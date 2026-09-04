#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "src/payload.h"
#include "src/test_util.h"

#ifndef F_GET_SEALS
#define F_GET_SEALS 1034
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008
#endif

int main() {  // NOLINT(bugprone-exception-escape)
  namespace fs = std::filesystem;
  const char* test_srcdir = std::getenv("TEST_SRCDIR");
  const char* test_workspace = std::getenv("TEST_WORKSPACE");
  const char* test_tmpdir = std::getenv("TEST_TMPDIR");
  llmcc::test::Expect(test_srcdir != nullptr && test_workspace != nullptr &&
                          test_tmpdir != nullptr,
                      "Bazel test environment");
  const fs::path executable = fs::path(test_srcdir) / test_workspace / "dist" /
                              "llm-cc-0.1-linux-x86_64";
  const fs::path runtime = fs::path(test_tmpdir) / "runtime";
  llmcc::test::ExpectEq(setenv("LLM_CC_RUNTIME_DIR", runtime.c_str(), 1), 0,
                        "runtime override is set");

  auto cuda = llmcc::PrepareEmbeddedPayloadFromExecutable(executable, "cuda");
  if (!cuda.has_value()) {
    llmcc::test::Expect(false, "CUDA payload is materialized in a memfd");
    return EXIT_FAILURE;
  }
  const llmcc::PreparedPayload& cuda_payload = cuda.value();
  llmcc::test::Expect(cuda_payload.backing_fd >= 0,
                      "CUDA payload is materialized in a memfd");
  struct stat cuda_status{};
  llmcc::test::Expect(fstat(cuda_payload.backing_fd, &cuda_status) == 0 &&
                          cuda_status.st_size > 0,
                      "CUDA memfd contains the embedded module");
  constexpr int kAllSeals =
      F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
  llmcc::test::ExpectEq(fcntl(cuda_payload.backing_fd, F_GET_SEALS), kAllSeals,
                        "CUDA memfd is immutable");
  close(cuda_payload.backing_fd);

  auto rocm = llmcc::PrepareEmbeddedPayloadFromExecutable(executable, "rocm");
  if (!rocm.has_value()) {
    llmcc::test::Expect(false,
                        "ROCm payload is extracted into its private cache");
    return EXIT_FAILURE;
  }
  llmcc::test::Expect(rocm->backing_fd < 0 && fs::is_regular_file(rocm->path),
                      "ROCm payload is extracted into its private cache");
  const fs::path module = rocm.value().path;
  const std::uintmax_t module_size = fs::file_size(module);
  const auto module_time = fs::last_write_time(module);

  rocm = llmcc::PrepareEmbeddedPayloadFromExecutable(executable, "rocm");
  llmcc::test::Expect(rocm.has_value() && rocm->path == module &&
                          fs::file_size(module) == module_size &&
                          fs::last_write_time(module) == module_time,
                      "valid ROCm cache is reused without rewriting files");

  {
    std::fstream corrupt(module,
                         std::ios::binary | std::ios::in | std::ios::out);
    char first = 0;
    corrupt.read(&first, 1);
    first ^= 0x7f;
    corrupt.seekp(0);
    corrupt.write(&first, 1);
  }
  rocm = llmcc::PrepareEmbeddedPayloadFromExecutable(executable, "rocm");
  llmcc::test::Expect(rocm.has_value() && rocm->path == module &&
                          fs::file_size(module) == module_size,
                      "corrupted ROCm cache is rebuilt atomically");
  return 0;
}
