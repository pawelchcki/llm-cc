#include "src/analyze.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/test_util.h"

namespace {

class FakeProvider : public llmcc::EntropyProvider {
 public:
  explicit FakeProvider(int& calls) : calls_(calls) {}

  std::vector<llmcc::EntropyRecord> Score(std::string_view source) override {
    ++calls_;
    if (source.empty()) {
      return {};
    }
    return {
        {.position = 0, .bytes = std::string(source), .entropy = std::nullopt}};
  }

 private:
  int& calls_;
};

class LineEntropyProvider : public llmcc::EntropyProvider {
 public:
  explicit LineEntropyProvider(std::vector<double> entropies)
      : entropies_(std::move(entropies)) {}

  std::vector<llmcc::EntropyRecord> Score(std::string_view source) override {
    std::vector<llmcc::EntropyRecord> records;
    std::size_t line = 0;
    for (std::size_t i = 0; i < source.size(); ++i) {
      records.push_back(
          {.position = i,
           .bytes = std::string(1, source[i]),
           .entropy = i == 0 ? std::nullopt
                             : std::optional<double>(entropies_.at(line))});
      if (source[i] == '\n' && line + 1 < entropies_.size()) {
        ++line;
      }
    }
    return records;
  }

 private:
  std::vector<double> entropies_;
};

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  namespace fs = std::filesystem;
  const char* temporary = std::getenv("TEST_TMPDIR");
  llmcc::test::Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const fs::path repository = fs::path(temporary) / "repo";
  const fs::path model = fs::path(temporary) / "model.gguf";
  fs::create_directories(repository);
  std::ofstream(model) << "model";
  const auto identity = llmcc::InspectModel(model, "abi", "cpu", 100);
  const llmcc::DiscoveredSource first{.path = repository / "a.rs",
                                      .language = llmcc::Language::kRust,
                                      .repository = repository};
  const llmcc::DiscoveredSource second{.path = repository / "b.rs",
                                       .language = llmcc::Language::kRust,
                                       .repository = repository};
  int factories = 0;
  int calls = 0;
  llmcc::ProjectAnalyzer analyzer(
      {.model = identity}, [&]() -> std::unique_ptr<llmcc::EntropyProvider> {
        ++factories;
        return std::make_unique<FakeProvider>(calls);
      });
  static_cast<void>(analyzer.AnalyzeFile(first, "fn a() {}\n"));
  static_cast<void>(analyzer.AnalyzeFile(second, "fn b() {}\n"));
  llmcc::test::ExpectEq(factories, 1, "two misses create one scorer");
  llmcc::test::ExpectEq(calls, 2, "both misses are scored");

  int hit_factories = 0;
  llmcc::ProjectAnalyzer cached(
      {.model = identity,
       .tau_rule = {.kind = llmcc::TauRule::Kind::kPercentile, .value = 90.0},
       .alpha = 0.2},
      [&]() -> std::unique_ptr<llmcc::EntropyProvider> {
        ++hit_factories;
        throw std::runtime_error("cache-only run loaded scorer");
      });
  const auto first_hit = cached.AnalyzeFile(first, "fn a() {}\n");
  const auto second_hit = cached.AnalyzeFile(second, "fn b() {}\n");
  llmcc::test::Expect(
      first_hit.entropy_cache_hit && second_hit.entropy_cache_hit,
      "alpha and percentile reuse cached entropy");
  llmcc::test::ExpectEq(hit_factories, 0, "cache hits load no scorer");

  const llmcc::DiscoveredSource miss{.path = repository / "0-miss.rs",
                                     .language = llmcc::Language::kRust,
                                     .repository = repository};
  int failing_factories = 0;
  llmcc::ProjectAnalyzer partially_cached(
      {.model = identity}, [&]() -> std::unique_ptr<llmcc::EntropyProvider> {
        ++failing_factories;
        throw std::runtime_error("model load failed");
      });
  bool initialization_failed = false;
  try {
    static_cast<void>(partially_cached.AnalyzeFile(miss, "fn miss() {}\n"));
  } catch (const llmcc::ScorerInitializationError&) {
    initialization_failed = true;
  }
  llmcc::test::Expect(initialization_failed,
                      "cache miss reports scorer initialization failure");
  llmcc::test::Expect(
      partially_cached.AnalyzeFile(second, "fn b() {}\n").entropy_cache_hit,
      "cache hit remains available after initialization failure");
  llmcc::test::ExpectEq(failing_factories, 1,
                        "failed scorer initialization is not retried");

  const llmcc::DiscoveredSource namespace_source{
      .path = repository / "namespace.cc",
      .language = llmcc::Language::kCpp,
      .repository = std::nullopt};
  const std::string namespace_contents =
      "namespace example {\n"
      "int calculate() {\n"
      "  return 1;\n"
      "}\n"
      "}\n";
  llmcc::ProjectAnalyzer percentile_analyzer(
      {.model = identity,
       .tau_rule = {.kind = llmcc::TauRule::Kind::kPercentile, .value = 100.0},
       .cache = false},
      []() {
        return std::make_unique<LineEntropyProvider>(
            std::vector<double>{10.0, 1.0, 1.0, 1.0, 10.0, 10.0});
      });
  const auto namespace_result =
      percentile_analyzer.AnalyzeFile(namespace_source, namespace_contents);
  llmcc::test::ExpectEq(namespace_result.analysis.tau, 10.0,
                        "file percentile resolves once");
  llmcc::test::ExpectEq(namespace_result.functions.size(), std::size_t{1},
                        "function nested in namespace is scored");
  llmcc::test::ExpectEq(namespace_result.functions[0].name,
                        std::string("calculate"), "nested function name");
  llmcc::test::ExpectEq(
      namespace_result.functions[0].metrics.high_entropy_tokens,
      std::uint64_t{0}, "function uses file tau rather than own percentile");

  const llmcc::DiscoveredSource commented_source{
      .path = repository / "commented.cc",
      .language = llmcc::Language::kCpp,
      .repository = std::nullopt};
  const std::string commented_contents =
      "// a removed comment\n"
      "int hot() {\n"
      "  return 7;\n"
      "}\n";
  llmcc::ProjectAnalyzer hotspot_analyzer(
      {.model = identity,
       .tau_rule = {.kind = llmcc::TauRule::Kind::kAbsolute, .value = 5.0},
       .cache = false,
       .hotspots = 1},
      []() {
        return std::make_unique<LineEntropyProvider>(
            std::vector<double>{0.1, 0.2, 9.0, 0.3, 0.1});
      });
  const auto hotspot_result =
      hotspot_analyzer.AnalyzeFile(commented_source, commented_contents);
  llmcc::test::ExpectEq(hotspot_result.hotspots.size(), std::size_t{1},
                        "hotspot limit applied");
  llmcc::test::ExpectEq(hotspot_result.hotspots[0].line, std::size_t{3},
                        "comment stripping preserves hotspot line number");
  return 0;
}
