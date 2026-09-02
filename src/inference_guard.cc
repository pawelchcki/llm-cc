#include "src/inference_guard.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace llmcc {
namespace {

std::filesystem::path LockPath(std::string_view backend) {
  return std::filesystem::temp_directory_path() /
         ("llm-cc-" + std::to_string(getuid()) + "-" + std::string(backend) +
          ".lock");
}

std::runtime_error SystemError(std::string_view operation) {
  return std::runtime_error(std::string(operation) + ": " +
                            std::strerror(errno));
}

}  // namespace

InferenceGuard::InferenceGuard(std::string_view backend) {
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
  std::cerr << "warning: waiting for another llm-cc process to release the "
            << backend << " inference device\n";
  while (flock(descriptor_, LOCK_EX) != 0) {
    if (errno != EINTR) {
      const auto error =
          SystemError("cannot acquire inference lock " + path.string());
      close(descriptor_);
      descriptor_ = -1;
      throw error;
    }
  }
}

InferenceGuard::~InferenceGuard() {
  if (descriptor_ >= 0) {
    close(descriptor_);
  }
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
