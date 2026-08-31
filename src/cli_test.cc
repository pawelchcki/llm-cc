#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

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
  const fs::path output = fs::path(test_tmpdir) / "analysis.json";

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

  const std::string command =
      Quote(binary) + " " + Quote(fixtures / "realistic.rs") +
      " --entropy-jsonl " + Quote(fixtures / "realistic.jsonl") + " >" +
      Quote(output);
  llmcc::test::ExpectEq(Run(command), 0, "realistic fixture analysis succeeds");
  const nlohmann::json actual = nlohmann::json::parse(Read(output));
  const nlohmann::json expected =
      nlohmann::json::parse(Read(fixtures / "realistic.expected.json"));
  llmcc::test::ExpectEq(actual, expected, "realistic output matches golden");

  const fs::path entropy_backend_error =
      fs::path(test_tmpdir) / "entropy-backend-error.txt";
  const std::string entropy_backend_command =
      Quote(binary) + " " + Quote(fixtures / "realistic.rs") +
      " --entropy-jsonl " + Quote(fixtures / "realistic.jsonl") +
      " --backend auto 2>" + Quote(entropy_backend_error);
  llmcc::test::Expect(Run(entropy_backend_command) != 0,
                      "backend option is rejected for JSONL analysis");
  llmcc::test::Expect(
      Read(entropy_backend_error).find("cannot be used with --entropy-jsonl") !=
          std::string::npos,
      "JSONL backend rejection is explained");

  if (fs::exists("/dev/full")) {
    const fs::path write_error = fs::path(test_tmpdir) / "write-error.txt";
    const std::string write_error_command =
        Quote(binary) + " " + Quote(fixtures / "realistic.rs") +
        " --entropy-jsonl " + Quote(fixtures / "realistic.jsonl") +
        " >/dev/full 2>" + Quote(write_error);
    llmcc::test::Expect(Run(write_error_command) != 0,
                        "analysis output failures are reported");
    llmcc::test::Expect(
        Read(write_error).find("failed to write output") != std::string::npos,
        "analysis output failure is explained");
  }

  const fs::path sample_output = fs::path(test_tmpdir) / "sample.json";
  const std::string sample_command =
      Quote(binary) + " " + Quote(fixtures / "sample.rs") + " --lang rust " +
      "--entropy-jsonl " + Quote(fixtures / "sample.jsonl") + " >" +
      Quote(sample_output);
  llmcc::test::ExpectEq(Run(sample_command), 0,
                        "sample Rust analysis succeeds");
  const nlohmann::json sample = nlohmann::json::parse(Read(sample_output));
  llmcc::test::Expect(std::abs(sample["tau"].get<double>() - 0.736) < 1e-12,
                      "sample tau");
  llmcc::test::Expect(std::abs(sample["llm_cc"].get<double>() - 1.4) < 1e-12,
                      "sample score");
  llmcc::test::ExpectEq(sample["total_branch"].get<int>(), 1,
                        "sample branch total");

  const fs::path cpp_output = fs::path(test_tmpdir) / "sample-cpp.json";
  const std::string cpp_command =
      Quote(binary) + " " + Quote(fixtures / "sample.cpp") +
      " --entropy-jsonl " + Quote(fixtures / "sample_cpp.jsonl") + " >" +
      Quote(cpp_output);
  llmcc::test::ExpectEq(Run(cpp_command), 0, "sample C++ analysis succeeds");
  const nlohmann::json cpp = nlohmann::json::parse(Read(cpp_output));
  llmcc::test::Expect(std::abs(cpp["tau"].get<double>() - 0.535) < 1e-12,
                      "C++ sample tau");
  llmcc::test::Expect(!cpp["units"].empty(), "C++ sample has units");

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

  return 0;
}
