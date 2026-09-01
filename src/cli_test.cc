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

  const fs::path score_help = fs::path(test_tmpdir) / "score-help.txt";
  llmcc::test::ExpectEq(
      Run(Quote(binary) + " score --help 2>" + Quote(score_help)), 0,
      "score help succeeds");
  llmcc::test::Expect(
      Read(score_help).find("default: 131072") != std::string::npos,
      "score advertises the large default context");

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

  const fs::path removed_option_error =
      fs::path(test_tmpdir) / "removed-option-error.txt";
  const std::string removed_option_command =
      Quote(binary) + " " + Quote(fixtures / "sample.rs") +
      " --entropy-jsonl " + Quote(fixtures / "sample.jsonl") + " 2>" +
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
  Write(source, "fn main() { println!(\"cached\"); }\n");
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
  llmcc::WriteEntropyCache(
      repository, preprocessed, identity,
      std::vector<llmcc::EntropyRecord>{
          {.position = 0, .bytes = preprocessed, .entropy = std::nullopt}});
  const fs::path analysis_output = fs::path(test_tmpdir) / "analysis.jsonl";
  const std::string analysis_command = Quote(binary) + " " + Quote(source) +
                                       " --model " + Quote(fake_model) + " >" +
                                       Quote(analysis_output);
  llmcc::test::ExpectEq(Run(analysis_command), 0,
                        "cache-only analysis succeeds without model load");
  std::istringstream lines(Read(analysis_output));
  std::vector<nlohmann::json> events;
  for (std::string line; std::getline(lines, line);) {
    events.push_back(nlohmann::json::parse(line));
  }
  llmcc::test::ExpectEq(events.size(), std::size_t{5},
                        "analysis emits five JSONL events");
  llmcc::test::Expect(
      events[0]["type"] == "start" && events[1]["type"] == "configuration" &&
          events[2]["type"] == "file_start" && events[3]["type"] == "file" &&
          events[4]["type"] == "totals",
      "analysis events are ordered");
  llmcc::test::Expect(events[3]["entropy_cache_hit"].get<bool>(),
                      "file reports entropy cache hit");
  llmcc::test::ExpectEq(events[4]["analyzed"].get<std::uint64_t>(),
                        std::uint64_t{1}, "totals report analyzed file");

  const fs::path status_output = fs::path(test_tmpdir) / "status.json";
  llmcc::test::ExpectEq(
      Run(Quote(binary) + " cache status " + Quote(repository) +
          " --format json >" + Quote(status_output)),
      0, "cache status succeeds");
  const nlohmann::json status = nlohmann::json::parse(Read(status_output));
  llmcc::test::ExpectEq(status["entries"].get<std::uint64_t>(),
                        std::uint64_t{1}, "cache status counts entries");

  return 0;
}
