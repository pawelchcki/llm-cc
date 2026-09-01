#include "src/analyze.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

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
      {.model = identity, .tau_percentile = 90.0, .alpha = 0.2},
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
  return 0;
}
