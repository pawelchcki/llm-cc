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
#include <vector>

#include "src/payload.h"

#if defined(__linux__)
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace llmcc {
namespace {

#if defined(LLM_CC_DYNAMIC_BACKENDS)
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
};

LoadedPlugin LoadPlugin(BackendKind backend, bool required) {
#if defined(__linux__)
  void* driver_handle = nullptr;
  if (backend == BackendKind::kCuda) {
    driver_handle = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (driver_handle == nullptr) {
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
  if (std::optional<PreparedPayload> embedded =
          PrepareEmbeddedPayload(BackendName(backend));
      embedded.has_value()) {
    if (ggml_backend_reg_t registry = ggml_backend_load(embedded->path.c_str());
        registry != nullptr) {
      return {.backend = backend,
              .registry = registry,
              .backing_fd = embedded->backing_fd,
              .driver_handle = driver_handle};
    }
#if defined(__linux__)
    if (embedded->backing_fd >= 0) {
      close(embedded->backing_fd);
    }
    if (driver_handle != nullptr) {
      dlclose(driver_handle);
    }
#endif
    if (required) {
      throw std::runtime_error("could not load embedded " +
                               std::string(BackendName(backend)) + " backend");
    }
    return {.backend = backend, .registry = nullptr};
  }
  std::vector<std::filesystem::path> candidates = PluginCandidates(backend);
  std::error_code error;
  for (const auto& candidate : candidates) {
    if (!std::filesystem::is_regular_file(candidate, error)) {
      error.clear();
      continue;
    }
    if (ggml_backend_reg_t registry = ggml_backend_load(candidate.c_str());
        registry != nullptr) {
      return {.backend = backend,
              .registry = registry,
              .driver_handle = driver_handle};
    }
    if (required) {
#if defined(__linux__)
      if (driver_handle != nullptr) {
        dlclose(driver_handle);
      }
#endif
      throw std::runtime_error("could not load " +
                               std::string(BackendName(backend)) +
                               " backend plugin: " + candidate.string());
    }
  }
#if defined(__linux__)
  if (driver_handle != nullptr) {
    dlclose(driver_handle);
  }
#endif
  if (required) {
    std::string message = "missing " + std::string(BackendName(backend)) +
                          " backend plugin " + std::string(PluginName(backend));
    if (!candidates.empty()) {
      message += " (searched beside the executable and in Bazel runfiles)";
    }
    throw std::runtime_error(message);
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

BackendRuntime::BackendRuntime(BackendKind requested, std::int32_t gpu_layers) {
#if defined(LLM_CC_DYNAMIC_BACKENDS)
  std::array<LoadedPlugin, 2> gpu_plugins{};
  std::size_t gpu_count = 0;
  if (requested == BackendKind::kCuda ||
      (gpu_layers != 0 && requested == BackendKind::kAuto)) {
    gpu_plugins[gpu_count++] =
        LoadPlugin(BackendKind::kCuda, requested == BackendKind::kCuda);
  }
  if (requested == BackendKind::kRocm ||
      (gpu_layers != 0 && requested == BackendKind::kAuto)) {
    gpu_plugins[gpu_count++] =
        LoadPlugin(BackendKind::kRocm, requested == BackendKind::kRocm);
  }
  selected_ = SelectBackend(
      requested, gpu_layers,
      Inventory(std::span<const LoadedPlugin>(gpu_plugins.data(), gpu_count)));
  for (std::size_t index = 0; index < gpu_count; ++index) {
    if (gpu_plugins[index].registry != nullptr &&
        gpu_plugins[index].backend != selected_) {
      UnloadPlugin(gpu_plugins[index]);
    }
  }
#else
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

BackendRuntime::~BackendRuntime() { llama_backend_free(); }

}  // namespace llmcc
