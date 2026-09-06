#include "src/inference_guard.h"

#include "src/progress.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace llmcc {
namespace {

#if !defined(_WIN32)
std::filesystem::path LockPath(std::string_view backend) {
  return std::filesystem::temp_directory_path() /
         ("llm-cc-" + std::to_string(getuid()) + "-" + std::string(backend) +
          ".lock");
}

std::runtime_error SystemError(std::string_view operation) {
  return std::runtime_error(std::string(operation) + ": " +
                            std::strerror(errno));
}
#endif

}  // namespace

InferenceGuard::InferenceGuard(std::string_view backend) {
#if defined(_WIN32)
  static_cast<void>(backend);
#else
  if (backend == "cpu") {
    return;
  }
  const std::filesystem::path path = LockPath(backend);
  descriptor_ =
      open(path.c_str(), O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_RDWR, 0600);
  if (descriptor_ < 0) {
    throw SystemError("cannot open inference lock " + path.string());
  }
  if (flock(descriptor_, LOCK_EX | LOCK_NB) == 0) {
    return;
  }
  if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
    const auto error =
        SystemError("cannot acquire inference lock " + path.string());
    close(descriptor_);
    descriptor_ = -1;
    throw error;
  }
  ReportPhase("waiting for GPU inference lock");
  ReportWarning("waiting for another llm-cc process to release the " +
                std::string(backend) + " inference device");
  while (flock(descriptor_, LOCK_EX) != 0) {
    if (errno != EINTR) {
      const auto error =
          SystemError("cannot acquire inference lock " + path.string());
      close(descriptor_);
      descriptor_ = -1;
      throw error;
    }
  }
#endif
}

InferenceGuard::~InferenceGuard() {
#if !defined(_WIN32)
  if (descriptor_ >= 0) {
    close(descriptor_);
  }
#endif
}

std::optional<std::string> CodexSandboxGpuWarning(
    std::optional<std::uint64_t> gpu_available, const char* sandbox) {
  if (!gpu_available.has_value() || *gpu_available != 0 || sandbox == nullptr ||
      *sandbox == '\0') {
    return std::nullopt;
  }
  return "Codex sandbox detected while the GPU reports no available memory; "
         "allow llm-cc to run outside the sandbox for accelerator access";
}

}  // namespace llmcc
