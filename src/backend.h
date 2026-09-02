#ifndef LLM_CC_BACKEND_H_
#define LLM_CC_BACKEND_H_

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace llmcc {

enum class BackendKind : std::uint8_t { kAuto, kCpu, kCuda, kRocm };

struct BackendDevice {
  BackendKind backend;
  std::uint64_t free_memory;
};

BackendKind ParseBackend(std::string_view value);
std::string_view BackendName(BackendKind backend);
BackendKind SelectBackend(BackendKind requested, std::int32_t gpu_layers,
                          std::span<const BackendDevice> devices);

// Loads exactly the backend plugins needed by this inference invocation. The
// object must outlive all llama.cpp objects created by the caller.
class BackendRuntime {
 public:
  BackendRuntime(BackendKind requested, std::int32_t gpu_layers);
  BackendRuntime(const BackendRuntime&) = delete;
  BackendRuntime& operator=(const BackendRuntime&) = delete;
  ~BackendRuntime();

  BackendKind selected() const { return selected_; }

 private:
  BackendKind selected_ = BackendKind::kCpu;
  void* plugin_registry_ = nullptr;
  int plugin_backing_fd_ = -1;
  void* driver_handle_ = nullptr;
};

}  // namespace llmcc

#endif  // LLM_CC_BACKEND_H_
