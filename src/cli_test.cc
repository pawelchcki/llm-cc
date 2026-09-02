#include <sys/wait.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "src/entropy_cache.h"
#include "src/lang.h"
#include "src/test_util.h"

namespace {

std::string Quote(const std::filesystem::path& path) {
  std::string result = "'";
  for (char character : path.string()) {
    result += character == '\'' ? "'\\''" : std::string(1, character);
  }
  return result + "'";
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}

int Run(const std::string& command) {
  return std::system(command.c_str());  // NOLINT(bugprone-command-processor)
}

void Write(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::vector<nlohmann::json> ReadEvents(const std::filesystem::path& path) {
  std::istringstream lines(Read(path));
  std::vector<nlohmann::json> events;
  for (std::string line; std::getline(lines, line);) {
    events.push_back(nlohmann::json::parse(line));
  }
  return events;
}

const nlohmann::json& FileEvent(const std::vector<nlohmann::json>& events) {
  for (const nlohmann::json& event : events) {
    if (event.value("type", "") == "file") {
      return event;
    }
  }
  throw std::runtime_error("file event not found");
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  namespace fs = std::filesystem;
  const char* test_srcdir = std::getenv("TEST_SRCDIR");
  const char* test_workspace = std::getenv("TEST_WORKSPACE");
  const char* test_tmpdir = std::getenv("TEST_TMPDIR");
  llmcc::test::Expect(test_srcdir != nullptr && test_workspace != nullptr &&
                          test_tmpdir != nullptr,
                      "Bazel test environment");
  const fs::path root = fs::path(test_srcdir) / test_workspace;
  const fs::path binary = root / "llm-cc";
  const fs::path fixtures = root / "testdata/cli";

  const fs::path analyze_help = fs::path(test_tmpdir) / "analyze-help.txt";
  llmcc::test::ExpectEq(Run(Quote(binary) + " --help 2>" + Quote(analyze_help)),
                        0, "analysis help succeeds");
  llmcc::test::Expect(
      Read(analyze_help).find("default: 131072") != std::string::npos,
      "analysis advertises the large default context");
  llmcc::test::Expect(
      Read(analyze_help).find("auto, cpu, cuda, or rocm") != std::string::npos,
      "analysis documents runtime backend selection");

  const fs::path score_help = fs::path(test_tmpdir) / "score-help.txt";
  llmcc::test::ExpectEq(
      Run(Quote(binary) + " score --help 2>" + Quote(score_help)), 0,
      "score help succeeds");
  llmcc::test::Expect(
      Read(score_help).find("default: 131072") != std::string::npos,
      "score advertises the large default context");
  llmcc::test::Expect(
      Read(score_help).find("auto, cpu, cuda, or rocm") != std::string::npos,
      "score documents runtime backend selection");

  const fs::path backend_error = fs::path(test_tmpdir) / "backend-error.txt";
  const std::string cpu_gpu_command =
      Quote(binary) +
      " score --model missing.gguf --prompt x --backend cpu "
      "--gpu-layers 1 2>" +
      Quote(backend_error);
  llmcc::test::Expect(Run(cpu_gpu_command) != 0,
                      "CPU backend rejects GPU offload");
  llmcc::test::Expect(
      Read(backend_error).find("--backend cpu") != std::string::npos,
      "CPU backend rejection is explained");

  const fs::path empty_repository = fs::path(test_tmpdir) / "empty-repository";
  fs::create_directories(empty_repository);
  llmcc::test::ExpectEq(Run("git -C " + Quote(empty_repository) + " init -q"),
                        0, "empty analysis repository initialized");
  Write(empty_repository / "only.h", "int declaration;\n");
  const fs::path empty_output = fs::path(test_tmpdir) / "empty.jsonl";
  llmcc::test::ExpectEq(Run(Quote(binary) + " " + Quote(empty_repository) +
                            " --no-download >" + Quote(empty_output)),
                        0,
                        "empty discovery does not resolve or download a model");
  std::istringstream empty_lines(Read(empty_output));
  std::vector<nlohmann::json> empty_events;
  for (std::string line; std::getline(empty_lines, line);) {
    empty_events.push_back(nlohmann::json::parse(line));
  }
  llmcc::test::Expect(
      empty_events.size() == 4 && empty_events[0]["type"] == "start" &&
          empty_events[1]["type"] == "configuration" &&
          empty_events[2]["type"] == "warning" &&
          empty_events[3]["type"] == "totals" &&
          empty_events[3]["discovered"] == 0,
      "empty discovery emits a complete zero-file event stream");
  const fs::path invalid_options_error =
      fs::path(test_tmpdir) / "invalid-options.txt";
  llmcc::test::Expect(Run(Quote(binary) + " " + Quote(empty_repository) +
                          " --alpha 2 2>" + Quote(invalid_options_error)) != 0,
                      "empty discovery rejects invalid analysis parameters");
  llmcc::test::Expect(
      Read(invalid_options_error)
              .find("--alpha must be finite and between 0 and 1") !=
          std::string::npos,
      "invalid global parameter is explained");

  const fs::path removed_option_error =
      fs::path(test_tmpdir) / "removed-option-error.txt";
  const std::string removed_option_command =
      Quote(binary) + " " + Quote(fixtures / "sample.rs") +
      " --entropy-jsonl " +
      Quote(fs::path(test_tmpdir) / "removed-entropy.jsonl") + " 2>" +
      Quote(removed_option_error);
  llmcc::test::Expect(Run(removed_option_command) != 0,
                      "removed entropy option is rejected");
  llmcc::test::Expect(
      Read(removed_option_error).find("unknown option") != std::string::npos,
      "removed entropy option reports an unknown option");

  const fs::path cache = fs::path(test_tmpdir) / "model-cache";
  fs::create_directories(cache);
  Write(cache / "alpha.gguf", std::string(1024, '\0'));
  Write(cache / "beta.gguf", std::string(2048, '\0'));
  Write(cache / "ignored.txt", "not a model");
  Write(cache / "models.json",
        R"({"alpha.gguf":{"downloaded_at":0,"last_used_at":86400}})");
  const fs::path list_output = fs::path(test_tmpdir) / "model-list.txt";
  const std::string list_command = "LLM_CC_CACHE_DIR=" + Quote(cache) + " " +
                                   Quote(binary) + " models list >" +
                                   Quote(list_output);
  llmcc::test::ExpectEq(Run(list_command), 0, "models list succeeds");
  const std::string listing = Read(list_output);
  llmcc::test::Expect(
      listing.find("alpha.gguf\t0.00 GiB\t1970-01-01 00:00:00 UTC\t"
                   "1970-01-02 00:00:00 UTC") != std::string::npos,
      "manifest timestamps listed");
  llmcc::test::Expect(
      listing.find("beta.gguf\t0.00 GiB\tnever\tnever") != std::string::npos,
      "untracked model listed");
  llmcc::test::Expect(listing.find("ignored.txt") == std::string::npos,
                      "non-model omitted");

  Write(cache / "remove.gguf", "model");
  Write(cache / "remove.gguf.partial", "partial");
  Write(cache / "keep.gguf", "keep");
  Write(
      cache / "models.json",
      R"({"remove.gguf":{"downloaded_at":1},"keep.gguf":{"downloaded_at":3}})");
  const std::string remove_command = "LLM_CC_CACHE_DIR=" + Quote(cache) + " " +
                                     Quote(binary) +
                                     " models remove remove.gguf";
  llmcc::test::ExpectEq(Run(remove_command), 0, "models remove succeeds");
  llmcc::test::Expect(!fs::exists(cache / "remove.gguf") &&
                          !fs::exists(cache / "remove.gguf.partial") &&
                          fs::exists(cache / "keep.gguf"),
                      "model and partial removed without touching peers");
  const nlohmann::json manifest =
      nlohmann::json::parse(Read(cache / "models.json"));
  llmcc::test::Expect(
      !manifest.contains("remove.gguf") && manifest.contains("keep.gguf"),
      "removed model deleted from manifest");

  const fs::path error_output = fs::path(test_tmpdir) / "remove-error.txt";
  const std::string reject_command =
      "LLM_CC_CACHE_DIR=" + Quote(cache) + " " + Quote(binary) +
      " models remove ../model.gguf 2>" + Quote(error_output);
  llmcc::test::Expect(Run(reject_command) != 0, "model paths rejected");
  llmcc::test::Expect(
      Read(error_output).find("bare file name") != std::string::npos,
      "path rejection is explained");

  const fs::path path_output = fs::path(test_tmpdir) / "cache-path.txt";
  const std::string models_command = "LLM_CC_CACHE_DIR=" + Quote(cache) + " " +
                                     Quote(binary) + " models path >" +
                                     Quote(path_output);
  llmcc::test::ExpectEq(Run(models_command), 0, "models path succeeds");
  llmcc::test::ExpectEq(Read(path_output), cache.string() + "\n",
                        "models path honors override");

  const fs::path repository = fs::path(test_tmpdir) / "analysis-repository";
  fs::create_directories(repository);
  llmcc::test::ExpectEq(Run("git -C " + Quote(repository) + " init -q"), 0,
                        "analysis test repository initialized");
  const fs::path source = repository / "source.rs";
  const fs::path fake_model = fs::path(test_tmpdir) / "fake.gguf";
  Write(source,
        "fn main() {\n"
        "  println!(\"cached\x1b[31m\"); // \rforged\n"
        "}\n");
  Write(fake_model, "not model weights");
  const auto [preprocessed, offsets] =
      llmcc::StripComments(Read(source), llmcc::Language::kRust);
  static_cast<void>(offsets);
#ifdef LLMCC_TEST_BACKEND_METAL
  constexpr std::string_view backend = "metal";
#elif defined LLMCC_TEST_BACKEND_CUDA
  constexpr std::string_view backend = "cuda";
#elif defined LLMCC_TEST_BACKEND_ROCM
  constexpr std::string_view backend = "rocm";
#else
  constexpr std::string_view backend = "cpu";
#endif
  const auto identity = llmcc::InspectModel(
      fake_model,
      "llama.cpp-c589f0ed10c643678c4707dd160c21ac7633ebc0/entropy-v1", backend,
      128U * 1024U);
  const std::size_t first_newline = preprocessed.find('\n');
  const std::size_t second_newline = preprocessed.find('\n', first_newline + 1);
  llmcc::WriteEntropyCache(
      repository, preprocessed, identity,
      std::vector<llmcc::EntropyRecord>{
          {.position = 0,
           .bytes = preprocessed.substr(0, 1),
           .entropy = std::nullopt},
          {.position = 1,
           .bytes = preprocessed.substr(1, first_newline),
           .entropy = 0.2},
          {.position = 2,
           .bytes = preprocessed.substr(first_newline + 1,
                                        second_newline - first_newline),
           .entropy = 1.2},
          {.position = 3,
           .bytes = preprocessed.substr(second_newline + 1),
           .entropy = 0.4}});
  const fs::path analysis_output = fs::path(test_tmpdir) / "analysis.jsonl";
  const std::string analysis_command = Quote(binary) + " " + Quote(source) +
                                       " --model " + Quote(fake_model) + " >" +
                                       Quote(analysis_output);
  llmcc::test::ExpectEq(Run(analysis_command), 0,
                        "cache-only analysis succeeds without model load");
  const std::vector<nlohmann::json> events = ReadEvents(analysis_output);
  llmcc::test::ExpectEq(events.size(), std::size_t{5},
                        "analysis emits five JSONL events");
  llmcc::test::Expect(
      events[0]["type"] == "start" && events[1]["type"] == "configuration" &&
          events[2]["type"] == "file_start" && events[3]["type"] == "file" &&
          events[4]["type"] == "totals",
      "analysis events are ordered");
  llmcc::test::Expect(events[3]["entropy_cache_hit"].get<bool>(),
                      "file reports entropy cache hit");
  const nlohmann::json& file = events[3];
  for (std::string_view field :
       {"token_count", "high_entropy_tokens", "lmcc_per_token", "density",
        "mean_entropy", "tau_rule", "score", "score_mode", "functions",
        "hotspots"}) {
    llmcc::test::Expect(file.contains(field),
                        std::string("file contains ") + std::string(field));
  }
  llmcc::test::Expect(!file["functions"].empty(),
                      "file includes function scores");
  llmcc::test::Expect(!file["hotspots"].empty(),
                      "file includes hotspot scores");
  llmcc::test::ExpectEq(events[4]["analyzed"].get<std::uint64_t>(),
                        std::uint64_t{1}, "totals report analyzed file");

  const fs::path density_output = fs::path(test_tmpdir) / "density.jsonl";
  llmcc::test::ExpectEq(
      Run(analysis_command.substr(0, analysis_command.rfind('>')) +
          "--score density >" + Quote(density_output)),
      0, "density analysis succeeds");
  const std::vector<nlohmann::json> density_events = ReadEvents(density_output);
  const nlohmann::json& density_file = FileEvent(density_events);
  llmcc::test::Expect(density_file["score_mode"] == "density" &&
                          density_file["score"] == density_file["density"],
                      "density selects the density headline");
  nlohmann::json default_without_headline = file;
  nlohmann::json density_without_headline = density_file;
  default_without_headline.erase("score");
  default_without_headline.erase("score_mode");
  density_without_headline.erase("score");
  density_without_headline.erase("score_mode");
  for (nlohmann::json& function : default_without_headline["functions"]) {
    function.erase("score");
  }
  for (nlohmann::json& function : density_without_headline["functions"]) {
    function.erase("score");
  }
  llmcc::test::ExpectEq(density_without_headline, default_without_headline,
                        "score mode changes only headline fields");

  const fs::path text_output = fs::path(test_tmpdir) / "analysis.txt";
  llmcc::test::ExpectEq(
      Run(analysis_command.substr(0, analysis_command.rfind('>')) +
          "--format text >" + Quote(text_output)),
      0, "text analysis succeeds");
  const std::string text_analysis = Read(text_output);
  llmcc::test::Expect(text_analysis.find("  fn ") != std::string::npos,
                      "text output contains a function line");
  llmcc::test::Expect(text_analysis.find("  H=") != std::string::npos,
                      "text output contains a hotspot line");
  llmcc::test::Expect(text_analysis.find('\x1b') == std::string::npos &&
                          text_analysis.find('\r') == std::string::npos &&
                          text_analysis.find("\\x1B") != std::string::npos &&
                          text_analysis.find("\\x0D") != std::string::npos,
                      "text hotspots escape terminal control bytes");

  const fs::path unsafe_source = repository / std::string("unsafe\x1b[31m.rs");
  Write(unsafe_source, "fn unsafe_source() {}\n");
  const fs::path unsafe_error = fs::path(test_tmpdir) / "unsafe-error.txt";
  const std::string unsafe_command = Quote(binary) + " " + Quote(source) + " " +
                                     Quote(unsafe_source) + " --model " +
                                     Quote(fake_model) + " --format text 2>" +
                                     Quote(unsafe_error) + " >/dev/null";
  llmcc::test::Expect(Run(unsafe_command) != 0,
                      "uncached source reports a text-mode error");
  const std::string unsafe_diagnostic = Read(unsafe_error);
  llmcc::test::Expect(unsafe_diagnostic.find('\x1b') == std::string::npos &&
                          unsafe_diagnostic.find("\\x1B") != std::string::npos,
                      "text errors escape filename control bytes");

  const fs::path conversion_source = repository / "conversion.cc";
  const std::string conversion_contents =
      "struct Convertible {\n"
      "  explicit operator\n"
      "      bool() const { return true; }\n"
      "};\n";
  Write(conversion_source, conversion_contents);
  const auto [conversion_preprocessed, conversion_offsets] =
      llmcc::StripComments(conversion_contents, llmcc::Language::kCpp);
  static_cast<void>(conversion_offsets);
  llmcc::WriteEntropyCache(repository, conversion_preprocessed, identity,
                           std::vector<llmcc::EntropyRecord>{
                               {.position = 0,
                                .bytes = conversion_preprocessed.substr(0, 1),
                                .entropy = std::nullopt},
                               {.position = 1,
                                .bytes = conversion_preprocessed.substr(1),
                                .entropy = 0.2}});
  const fs::path conversion_output =
      fs::path(test_tmpdir) / "conversion-output.txt";
  const std::string conversion_command =
      Quote(binary) + " " + Quote(conversion_source) + " --model " +
      Quote(fake_model) + " --format text >" + Quote(conversion_output);
  llmcc::test::ExpectEq(Run(conversion_command), 0,
                        "multiline conversion operator analysis succeeds");
  const std::string conversion_text = Read(conversion_output);
  llmcc::test::Expect(
      conversion_text.find("operator\\x0A") != std::string::npos &&
          conversion_text.find("operator\n") == std::string::npos,
      "text function names escape embedded newlines");

  const fs::path no_hotspots_output =
      fs::path(test_tmpdir) / "no-hotspots.jsonl";
  llmcc::test::ExpectEq(
      Run(analysis_command.substr(0, analysis_command.rfind('>')) +
          "--hotspots 0 >" + Quote(no_hotspots_output)),
      0, "zero-hotspot analysis succeeds");
  llmcc::test::Expect(
      !FileEvent(ReadEvents(no_hotspots_output)).contains("hotspots"),
      "zero hotspots omits the array");

  const fs::path conflicting_tau_error =
      fs::path(test_tmpdir) / "conflicting-tau.txt";
  const int conflicting_tau =
      Run(Quote(binary) + " " + Quote(source) +
          " --tau 0.5 --tau-percentile 90 2>" + Quote(conflicting_tau_error));
  llmcc::test::Expect(
      WIFEXITED(conflicting_tau) && WEXITSTATUS(conflicting_tau) == 2,
      "conflicting tau options exit 2");

  const fs::path status_output = fs::path(test_tmpdir) / "status.json";
  llmcc::test::ExpectEq(
      Run(Quote(binary) + " cache status " + Quote(repository) +
          " --format json >" + Quote(status_output)),
      0, "cache status succeeds");
  const nlohmann::json status = nlohmann::json::parse(Read(status_output));
  llmcc::test::ExpectEq(status["entries"].get<std::uint64_t>(),
                        std::uint64_t{2}, "cache status counts entries");

  return 0;
}
