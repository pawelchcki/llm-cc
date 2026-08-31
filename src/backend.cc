#include "src/backend.h"

#include <ggml-backend.h>
#include <llama.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#if defined(__linux__)
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
    // `bazel-bin/llm-cc` and `bazel-bin/llama_cpp/lib/...` during development.
    candidates.push_back(executable->parent_path() / "llama_cpp" / "lib" /
                         file);
  }
  if (const char* runfiles = std::getenv("RUNFILES_DIR");
      runfiles != nullptr && *runfiles != '\0') {
    const std::filesystem::path root(runfiles);
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
};

LoadedPlugin LoadPlugin(BackendKind backend, bool required) {
  std::vector<std::filesystem::path> candidates = PluginCandidates(backend);
  std::error_code error;
  for (const auto& candidate : candidates) {
    if (!std::filesystem::is_regular_file(candidate, error)) {
      error.clear();
      continue;
    }
    if (ggml_backend_reg_t registry = ggml_backend_load(candidate.c_str());
        registry != nullptr) {
      return {.backend = backend, .registry = registry};
    }
    if (required) {
      throw std::runtime_error("could not load " +
                               std::string(BackendName(backend)) +
                               " backend plugin: " + candidate.string());
    }
  }
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

BackendKind ParseBackend(std::string_view value) {
  if (value == "auto") {
    return BackendKind::kAuto;
  }
  if (value == "cpu") {
    return BackendKind::kCpu;
  }
  if (value == "cuda") {
    return BackendKind::kCuda;
  }
  if (value == "rocm") {
    return BackendKind::kRocm;
  }
  throw std::invalid_argument("--backend expects auto, cpu, cuda, or rocm");
}

std::string_view BackendName(BackendKind backend) {
  switch (backend) {
    case BackendKind::kAuto:
      return "auto";
    case BackendKind::kCpu:
      return "cpu";
    case BackendKind::kCuda:
      return "cuda";
    case BackendKind::kRocm:
      return "rocm";
  }
  return "unknown";
}

BackendKind SelectBackend(BackendKind requested, std::int32_t gpu_layers,
                          std::span<const BackendDevice> devices) {
  if (gpu_layers < -1) {
    throw std::invalid_argument("--gpu-layers must be -1 or greater");
  }
  if (requested == BackendKind::kCpu && gpu_layers != 0) {
    throw std::invalid_argument(
        "--backend cpu cannot be used with nonzero --gpu-layers");
  }
  if (gpu_layers == 0 && requested == BackendKind::kAuto) {
    return BackendKind::kCpu;
  }

  std::uint64_t cuda_memory = 0;
  std::uint64_t rocm_memory = 0;
  bool has_cuda = false;
  bool has_rocm = false;
  for (const BackendDevice& device : devices) {
    std::uint64_t* aggregate = nullptr;
    bool* present = nullptr;
    if (device.backend == BackendKind::kCuda) {
      aggregate = &cuda_memory;
      present = &has_cuda;
    } else if (device.backend == BackendKind::kRocm) {
      aggregate = &rocm_memory;
      present = &has_rocm;
    } else {
      continue;
    }
    *present = true;
    *aggregate = device.free_memory >
                         std::numeric_limits<std::uint64_t>::max() - *aggregate
                     ? std::numeric_limits<std::uint64_t>::max()
                     : *aggregate + device.free_memory;
  }

  if (requested == BackendKind::kCuda) {
    if (!has_cuda) {
      throw std::runtime_error(
          "CUDA backend was requested, but no usable CUDA device was found");
    }
    return BackendKind::kCuda;
  }
  if (requested == BackendKind::kRocm) {
    if (!has_rocm) {
      throw std::runtime_error(
          "ROCm backend was requested, but no usable ROCm device was found");
    }
    return BackendKind::kRocm;
  }
  if (!has_cuda && !has_rocm) {
    throw std::runtime_error(
        "GPU offload was requested, but no usable CUDA or ROCm device was "
        "found");
  }
  if (has_cuda && (!has_rocm || cuda_memory >= rocm_memory)) {
    return BackendKind::kCuda;
  }
  return BackendKind::kRocm;
}

BackendRuntime::BackendRuntime(BackendKind requested, std::int32_t gpu_layers) {
#if defined(LLM_CC_DYNAMIC_BACKENDS)
  const LoadedPlugin cpu = LoadPlugin(BackendKind::kCpu, true);
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
      ggml_backend_unload(gpu_plugins[index].registry);
    }
  }
  static_cast<void>(cpu);
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
