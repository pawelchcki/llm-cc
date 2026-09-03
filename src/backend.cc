#include "src/backend.h"

#include <ggml-backend.h>
#include <llama.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "src/backend_fetch.h"
#include "src/payload.h"
#include "src/rocm_topology.h"

#if defined(__linux__)
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace llmcc {
namespace {

class MissingBackendPluginError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

constexpr std::string_view PluginName(BackendKind backend) {
  switch (backend) {
    case BackendKind::kCpu:
      return "libllm-cc-backend-cpu.so";
    case BackendKind::kCuda:
      return "libllm-cc-backend-cuda.so";
    case BackendKind::kRocm:
      return "libllm-cc-backend-rocm.so";
    case BackendKind::kAuto:
      break;
  }
  return {};
}

std::string BundleName(BackendKind backend) {
  return std::string(BackendName(backend)) + ".bundle";
}

std::string DistributionBundleName(BackendKind backend) {
  return "llm-cc-backend-" + std::string(BackendName(backend)) +
         "-linux-x86_64.bundle";
}

bool IsRegularFile(const std::filesystem::path& path) {
  std::error_code error;
  const bool regular = std::filesystem::is_regular_file(path, error);
  if (error == std::errc::no_such_file_or_directory ||
      error == std::errc::not_a_directory) {
    return false;
  }
  if (error) {
    throw std::runtime_error("could not inspect backend path " + path.string() +
                             ": " + error.message());
  }
  return regular;
}

void ValidateBackendDirectory(const std::filesystem::path& directory) {
  std::error_code error;
  if (std::filesystem::is_directory(directory, error)) {
    return;
  }
  std::string message =
      "backend directory is not a directory: " + directory.string();
  if (error) {
    message += ": " + error.message();
  }
  throw std::runtime_error(message);
}

#if defined(LLM_CC_DYNAMIC_BACKENDS)

std::optional<std::filesystem::path> ExecutablePath() {
#if defined(__linux__)
  std::vector<char> buffer(1024);
  for (;;) {
    const ssize_t count =
        readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (count < 0) {
      return std::nullopt;
    }
    if (static_cast<std::size_t>(count) < buffer.size()) {
      return std::filesystem::path(
          std::string(buffer.data(), static_cast<std::size_t>(count)));
    }
    buffer.resize(buffer.size() * 2);
  }
#else
  return std::nullopt;
#endif
}

std::vector<std::filesystem::path> PluginCandidates(BackendKind backend) {
  const std::filesystem::path file(PluginName(backend));
  std::vector<std::filesystem::path> candidates;
  if (const auto executable = ExecutablePath(); executable.has_value()) {
    candidates.push_back(executable->parent_path() / "lib" / file);
    candidates.push_back(executable->parent_path() / "external" /
                         "+http_archive+llama_cpp" / file);
  }
  if (const char* runfiles = std::getenv("RUNFILES_DIR");
      runfiles != nullptr && *runfiles != '\0') {
    const std::filesystem::path root(runfiles);
    candidates.push_back(root / "+http_archive+llama_cpp" / file);
    candidates.push_back(root / "_main" / "llama_cpp" / "lib" / file);
    if (const char* workspace = std::getenv("TEST_WORKSPACE");
        workspace != nullptr && *workspace != '\0') {
      candidates.push_back(root / workspace / "llama_cpp" / "lib" / file);
    }
  }
  return candidates;
}

struct LoadedPlugin {
  BackendKind backend;
  ggml_backend_reg_t registry;
  int backing_fd = -1;
  void* driver_handle = nullptr;
  bool missing_with_download_disabled = false;
};

std::string DownloadDisabledMessage(BackendKind backend) {
  const std::string name(BackendName(backend));
  return "missing " + name +
         " backend bundle while --no-download is set; run 'llm-cc backends "
         "fetch " +
         name + "' or pass --url to that command";
}

LoadedPlugin LoadPlugin(
    BackendKind backend, bool required,
    const std::optional<std::filesystem::path>& backend_directory,
    std::string_view version, bool no_download, bool fetch_backend) {
#if defined(__linux__)
  void* driver_handle = nullptr;
#endif
  const auto close_driver = [&]() {
#if defined(__linux__)
    if (driver_handle != nullptr) {
      dlclose(driver_handle);
      driver_handle = nullptr;
    }
#endif
  };
  std::optional<PreparedPayload> prepared;
  ResolvedBackendPlugin resolved;
  try {
    std::function<std::optional<ResolvedBackendPlugin>()> fetch;
    if (fetch_backend && !no_download) {
      fetch = [backend, version] {
        const std::filesystem::path path = FetchBackendBundle(
            {.name = BackendName(backend), .version = version});
        return std::optional<ResolvedBackendPlugin>(
            {{.source = BackendPluginSource::kBundle, .path = path}});
      };
    }
    resolved = ResolveBackendPlugin(
        backend, backend_directory, PluginCandidates(backend),
        [&]() {
          prepared = PrepareEmbeddedPayload(BackendName(backend));
          return prepared.has_value();
        },
        [] { return RuntimeRoot(); }, version, fetch);
    if (resolved.source == BackendPluginSource::kBundle) {
      prepared =
          PrepareEmbeddedPayloadFromFile(resolved.path, BackendName(backend));
      if (!prepared.has_value()) {
        throw std::runtime_error(
            "backend bundle does not begin with the expected " +
            std::string(BackendName(backend)) +
            " magic: " + resolved.path.string());
      }
    }
  } catch (const MissingBackendPluginError&) {
    close_driver();
    if (required) {
      if (no_download) {
        throw std::runtime_error(DownloadDisabledMessage(backend));
      }
      throw;
    }
    return {.backend = backend,
            .registry = nullptr,
            .missing_with_download_disabled = no_download};
  } catch (...) {
    close_driver();
    if (required) {
      throw;
    }
    return {.backend = backend, .registry = nullptr};
  }
#if defined(__linux__)
  if (backend == BackendKind::kCuda) {
    driver_handle = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (driver_handle == nullptr) {
      if (prepared.has_value() && prepared->backing_fd >= 0) {
        close(prepared->backing_fd);
      }
      if (required) {
        const char* error = dlerror();
        throw std::runtime_error(
            "could not load NVIDIA driver libcuda.so.1: " +
            std::string(error != nullptr ? error : "unknown loader error"));
      }
      return {.backend = backend, .registry = nullptr};
    }
  }
#endif
  const std::filesystem::path plugin_path =
      prepared.has_value() ? prepared->path : resolved.path;
  std::optional<RocmTopology> rocm_topology;
  if (backend == BackendKind::kRocm) {
    // This is intentionally delayed until the ROCm plugin is resolved and is
    // immediately about to be loaded. CPU-only invocations never probe KFD.
    rocm_topology = ConfigureRocmVisibility();
  }
  if (ggml_backend_reg_t registry = ggml_backend_load(plugin_path.c_str());
      registry != nullptr) {
    return {.backend = backend,
            .registry = registry,
            .backing_fd = prepared.has_value() ? prepared->backing_fd : -1,
            .driver_handle = driver_handle};
  }
#if defined(__linux__)
  if (prepared.has_value() && prepared->backing_fd >= 0) {
    close(prepared->backing_fd);
  }
#endif
  close_driver();
  if (required) {
    if (backend == BackendKind::kRocm && rocm_topology.has_value() &&
        rocm_topology->has_unsupported_device) {
      throw std::runtime_error(RocmUnsupportedSystemMessage(*rocm_topology));
    }
    if (resolved.source == BackendPluginSource::kEmbedded) {
      throw std::runtime_error("could not load embedded " +
                               std::string(BackendName(backend)) + " backend");
    }
    throw std::runtime_error("could not load " +
                             std::string(BackendName(backend)) +
                             " backend plugin: " + resolved.path.string());
  }
  return {.backend = backend, .registry = nullptr};
}

void UnloadPlugin(LoadedPlugin& plugin) {
  if (plugin.registry != nullptr) {
    ggml_backend_unload(plugin.registry);
    plugin.registry = nullptr;
  }
#if defined(__linux__)
  if (plugin.backing_fd >= 0) {
    close(plugin.backing_fd);
    plugin.backing_fd = -1;
  }
  if (plugin.driver_handle != nullptr) {
    dlclose(plugin.driver_handle);
    plugin.driver_handle = nullptr;
  }
#endif
}

std::vector<BackendDevice> Inventory(std::span<const LoadedPlugin> plugins) {
  std::vector<BackendDevice> devices;
  for (const LoadedPlugin& plugin : plugins) {
    if (plugin.registry == nullptr || plugin.backend == BackendKind::kCpu) {
      continue;
    }
    for (std::size_t index = 0; index < ggml_backend_dev_count(); ++index) {
      ggml_backend_dev_t device = ggml_backend_dev_get(index);
      const enum ggml_backend_dev_type type = ggml_backend_dev_type(device);
      if (ggml_backend_dev_backend_reg(device) != plugin.registry ||
          (type != GGML_BACKEND_DEVICE_TYPE_GPU &&
           type != GGML_BACKEND_DEVICE_TYPE_IGPU)) {
        continue;
      }
      std::size_t free_memory = 0;
      std::size_t total_memory = 0;
      ggml_backend_dev_memory(device, &free_memory, &total_memory);
      devices.push_back(
          {.backend = plugin.backend,
           .free_memory = static_cast<std::uint64_t>(free_memory)});
    }
  }
  return devices;
}
#endif

}  // namespace

ResolvedBackendPlugin ResolveBackendPlugin(
    BackendKind backend,
    const std::optional<std::filesystem::path>& backend_directory,
    std::span<const std::filesystem::path> runfile_candidates,
    const std::function<bool()>& has_embedded_payload,
    const std::function<std::filesystem::path()>& runtime_root,
    std::string_view version,
    const std::function<std::optional<ResolvedBackendPlugin>()>&
        fetch_backend) {
  if (backend != BackendKind::kCuda && backend != BackendKind::kRocm) {
    throw std::invalid_argument("only CUDA and ROCm plugins can be resolved");
  }

  // Resolution order:
  //   1. configured directory (standalone bundles before a raw plugin),
  //   2. the executable's embedded payload,
  //   3. beside-the-executable and Bazel runfiles candidates,
  //   4. the versioned runtime cache,
  //   5. the currently empty network-fetch seam below.
  if (backend_directory.has_value()) {
    ValidateBackendDirectory(*backend_directory);
    const std::array<std::pair<std::filesystem::path, BackendPluginSource>, 3>
        candidates = {{
            {*backend_directory / DistributionBundleName(backend),
             BackendPluginSource::kBundle},
            {*backend_directory / BundleName(backend),
             BackendPluginSource::kBundle},
            {*backend_directory / PluginName(backend),
             BackendPluginSource::kSharedLibrary},
        }};
    for (const auto& [path, source] : candidates) {
      if (IsRegularFile(path)) {
        return {.source = source, .path = path};
      }
    }
  }

  // Step 2 deliberately stays between the configured directory and all
  // development-tree paths so the historical fat-ELF behavior is unchanged.
  if (has_embedded_payload && has_embedded_payload()) {
    return {.source = BackendPluginSource::kEmbedded, .path = {}};
  }

  for (const std::filesystem::path& candidate : runfile_candidates) {
    if (IsRegularFile(candidate)) {
      return {.source = BackendPluginSource::kSharedLibrary, .path = candidate};
    }
  }

  const std::filesystem::path cached_bundle =
      runtime_root() / "backends" / version / BundleName(backend);
  if (IsRegularFile(cached_bundle)) {
    return {.source = BackendPluginSource::kBundle, .path = cached_bundle};
  }

  // Step 5 is deliberately invoked only after every local source misses.
  if (fetch_backend) {
    if (auto fetched = fetch_backend(); fetched.has_value()) {
      return *fetched;
    }
  }

  std::string message = "missing " + std::string(BackendName(backend)) +
                        " backend plugin " + std::string(PluginName(backend)) +
                        " (set --backend-dir or LLM_CC_BACKEND_DIR";
  if (backend_directory.has_value()) {
    message += "; searched backend directory " + backend_directory->string();
  }
  message +=
      ", the embedded executable, beside the executable and in Bazel "
      "runfiles, and runtime cache " +
      cached_bundle.string() + ")";
  throw MissingBackendPluginError(message);
}

BackendRuntime::BackendRuntime(
    BackendKind requested, std::int32_t gpu_layers, std::string_view version,
    const std::optional<std::filesystem::path>& backend_directory,
    bool no_download, bool fetch_backend) {
  if (gpu_layers < -1) {
    throw std::invalid_argument("--gpu-layers must be -1 or greater");
  }
#if defined(LLM_CC_DYNAMIC_BACKENDS)
  std::array<LoadedPlugin, 2> gpu_plugins{};
  std::size_t gpu_count = 0;
  const bool load_gpu = requested == BackendKind::kCuda ||
                        requested == BackendKind::kRocm ||
                        (gpu_layers != 0 && requested == BackendKind::kAuto);
  if (load_gpu && backend_directory.has_value()) {
    ValidateBackendDirectory(*backend_directory);
  }
  if (requested == BackendKind::kCuda ||
      (gpu_layers != 0 && requested == BackendKind::kAuto)) {
    gpu_plugins[gpu_count++] =
        LoadPlugin(BackendKind::kCuda, requested == BackendKind::kCuda,
                   backend_directory, version, no_download, fetch_backend);
  }
  if (requested == BackendKind::kRocm ||
      (gpu_layers != 0 && requested == BackendKind::kAuto)) {
    gpu_plugins[gpu_count++] =
        LoadPlugin(BackendKind::kRocm, requested == BackendKind::kRocm,
                   backend_directory, version, no_download, fetch_backend);
  }
  try {
    const std::vector<BackendDevice> devices =
        Inventory(std::span<const LoadedPlugin>(gpu_plugins.data(), gpu_count));
    if (devices.empty() && no_download) {
      const auto missing =
          std::find_if(gpu_plugins.begin(), gpu_plugins.begin() + gpu_count,
                       [](const LoadedPlugin& plugin) {
                         return plugin.missing_with_download_disabled;
                       });
      if (missing != gpu_plugins.begin() + gpu_count) {
        throw std::runtime_error(DownloadDisabledMessage(missing->backend));
      }
    }
    selected_ = SelectBackend(requested, gpu_layers, devices);
  } catch (...) {
    for (std::size_t index = 0; index < gpu_count; ++index) {
      UnloadPlugin(gpu_plugins[index]);
    }
    throw;
  }
  for (std::size_t index = 0; index < gpu_count; ++index) {
    LoadedPlugin& plugin = gpu_plugins[index];
    if (plugin.registry == nullptr) {
      continue;
    }
    if (plugin.backend != selected_) {
      UnloadPlugin(plugin);
      continue;
    }
    plugin_registry_ = plugin.registry;
    plugin_backing_fd_ = plugin.backing_fd;
    driver_handle_ = plugin.driver_handle;
    plugin.registry = nullptr;
    plugin.backing_fd = -1;
    plugin.driver_handle = nullptr;
  }
#else
  static_cast<void>(version);
  static_cast<void>(backend_directory);
  static_cast<void>(no_download);
  static_cast<void>(fetch_backend);
#if defined(LLM_CC_BUILTIN_GPU)
  if (requested == BackendKind::kCuda || requested == BackendKind::kRocm) {
    throw std::runtime_error(std::string(BackendName(requested)) +
                             " backend is not included in this build");
  }
  if (requested == BackendKind::kCpu && gpu_layers != 0) {
    throw std::invalid_argument(
        "--backend cpu cannot be used with nonzero --gpu-layers");
  }
  selected_ = requested;
#else
  selected_ = SelectBackend(requested, gpu_layers, {});
  if (selected_ != BackendKind::kCpu && requested != BackendKind::kAuto) {
    throw std::runtime_error(std::string(BackendName(selected_)) +
                             " backend is not included in this build");
  }
#endif
#endif
  llama_backend_init();
}

BackendRuntime::~BackendRuntime() {
  llama_backend_free();
#if defined(LLM_CC_DYNAMIC_BACKENDS)
  LoadedPlugin plugin{
      .backend = selected_,
      .registry = static_cast<ggml_backend_reg_t>(plugin_registry_),
      .backing_fd = plugin_backing_fd_,
      .driver_handle = driver_handle_,
  };
  UnloadPlugin(plugin);
#endif
}

}  // namespace llmcc
