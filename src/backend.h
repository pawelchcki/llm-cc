#ifndef LLM_CC_BACKEND_H_
#define LLM_CC_BACKEND_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
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

enum class BackendPluginSource : std::uint8_t {
  kBundle,
  kEmbedded,
  kSharedLibrary,
};

struct ResolvedBackendPlugin {
  BackendPluginSource source;
  std::filesystem::path path;
  bool payload_verified = false;
};

// Resolves a GPU plugin in production order. The callback keeps the embedded
// payload probe at step 2 without making filesystem-only unit tests load it.
// fetch_backend is the network-backed last resort at step 5.
ResolvedBackendPlugin ResolveBackendPlugin(
    BackendKind backend,
    const std::optional<std::filesystem::path>& backend_directory,
    std::span<const std::filesystem::path> runfile_candidates,
    const std::function<bool()>& has_embedded_payload,
    const std::function<std::filesystem::path()>& runtime_root,
    std::string_view version, std::string_view git_sha = {},
    const std::function<std::optional<ResolvedBackendPlugin>()>& fetch_backend =
        {});

// Loads exactly the backend plugins needed by this inference invocation. The
// object must outlive all llama.cpp objects created by the caller.
class BackendRuntime {
 public:
  BackendRuntime(BackendKind requested, std::int32_t gpu_layers,
                 std::string_view version,
                 const std::optional<std::filesystem::path>& backend_directory =
                     std::nullopt,
                 bool no_download = false, bool fetch_backend = true);
  BackendRuntime(const BackendRuntime&) = delete;
  BackendRuntime& operator=(const BackendRuntime&) = delete;
  ~BackendRuntime();

  [[nodiscard]] BackendKind selected() const { return selected_; }

 private:
  BackendKind selected_ = BackendKind::kCpu;
  void* plugin_registry_ = nullptr;
  int plugin_backing_fd_ = -1;
  void* driver_handle_ = nullptr;
};

}  // namespace llmcc

#endif  // LLM_CC_BACKEND_H_
