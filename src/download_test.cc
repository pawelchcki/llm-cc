#include "src/download.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "src/cache.h"
#include "src/test_util.h"

int main() {  // NOLINT(bugprone-exception-escape)
  namespace fs = std::filesystem;
  const fs::path root = std::getenv("TEST_TMPDIR");
  const fs::path target = root / "models/model.gguf";
  std::istringstream first("model data");
  llmcc::StreamDownload(first, target, 0, 10);
  std::ifstream completed(target, std::ios::binary);
  std::string contents((std::istreambuf_iterator<char>(completed)), {});
  llmcc::test::ExpectEq(contents, std::string("model data"),
                        "completed download renamed");

  const fs::path resumed = root / "resumed.gguf";
  std::ofstream(llmcc::PartialPath(resumed), std::ios::binary) << "first ";
  std::istringstream second("second");
  llmcc::StreamDownload(second, resumed, 6, 12);
  std::ifstream resumed_input(resumed, std::ios::binary);
  contents.assign(std::istreambuf_iterator<char>(resumed_input), {});
  llmcc::test::ExpectEq(contents, std::string("first second"),
                        "resume appends");
  const auto missing = llmcc::DownloadFailureMessage(
      "https://example.invalid/missing.bundle", 404, "not found", false);
  llmcc::test::Expect(
      missing.find("HTTP 404") != std::string::npos &&
          missing.find("https://example.invalid/missing.bundle") !=
              std::string::npos,
      "HTTP errors retain status and failing URL");
  const auto timed_out = llmcc::DownloadFailureMessage(
      "https://example.invalid/slow", 200, "too slow", true);
  llmcc::test::Expect(
      timed_out.find("15-second connection") != std::string::npos &&
          timed_out.find("60-second stalled-transfer") != std::string::npos &&
          timed_out.find("partial download preserved") != std::string::npos,
      "timeout diagnostic identifies limits and recovery");
  llmcc::test::ExpectEq(
      llmcc::SanitizeUrlForDiagnostic(
          "https://user:password@example.invalid/object/model.gguf?"
          "X-Amz-Signature=secret#fragment"),
      std::string("https://example.invalid/object/model.gguf"),
      "redirect diagnostics redact credentials, query, and fragment");
  const fs::path interrupted = root / "interrupted.gguf";
  std::istringstream truncated("part");
  bool failed = false;
  try {
    llmcc::StreamDownload(truncated, interrupted, 0, 10);
  } catch (const std::runtime_error&) {
    failed = true;
  }
  llmcc::test::Expect(failed && !fs::exists(interrupted) &&
                          fs::file_size(llmcc::PartialPath(interrupted)) == 4,
                      "failed transfer preserves partial bytes");
  return 0;
}
