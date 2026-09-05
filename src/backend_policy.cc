#include <limits>
#include <stdexcept>
#include <string_view>

#include "src/backend.h"

namespace llmcc {

ExecutionOptions ResolveExecutionOptions(BackendKind backend,
                                         std::optional<std::int32_t> gpu_layers,
                                         bool force_cpu) {
  if (gpu_layers && *gpu_layers < -1) {
    throw std::invalid_argument("--gpu-layers must be -1 or greater");
  }
  const bool accelerator =
      backend == BackendKind::kCuda || backend == BackendKind::kRocm;
  if ((force_cpu && accelerator) ||
      ((force_cpu || backend == BackendKind::kCpu) && gpu_layers &&
       *gpu_layers != 0) ||
      (accelerator && gpu_layers == 0)) {
    throw std::invalid_argument(
        "contradictory CPU and accelerator options (--force-cpu, --backend, "
        "--gpu-layers)");
  }
  if (force_cpu || backend == BackendKind::kCpu || gpu_layers == 0) {
    return {BackendKind::kCpu, 0};
  }
  return {backend, gpu_layers.value_or(-1)};
}

std::string CpuRecoveryCommand(int argc, char** argv, bool score) {
  std::string result = "CPU rerun: llm-cc";
  if (score) result += " score";
  const auto append_argument = [&](std::string_view arg) {
#ifdef _WIN32
    result += " \"";
    std::size_t backslashes = 0;
    for (char ch : arg) {
      if (ch == '\\') {
        ++backslashes;
        continue;
      }
      if (ch == '"') {
        result.append(backslashes * 2 + 1, '\\');
        result += ch;
        backslashes = 0;
        continue;
      }
      result.append(backslashes, '\\');
      backslashes = 0;
      result += static_cast<unsigned char>(ch) < 32 || ch == 127 ? '?' : ch;
    }
    // Backslashes before the closing quote must be doubled for the Windows
    // command-line parser to retain them in the argument.
    result.append(backslashes * 2, '\\');
    result += '"';
#else
    result += " '";
    for (char ch : arg) {
      if (ch == '\'')
        result += "'\\''";
      else if (static_cast<unsigned char>(ch) < 32 || ch == 127)
        result += '?';
      else
        result += ch;
    }
    result += "'";
#endif
  };
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--backend" || arg == "--gpu-layers" || arg == "--backend-dir" ||
        arg == "--entropy-reduction") {
      ++i;
      continue;
    }
    if (arg == "--force-cpu") continue;
    append_argument(arg);
    const bool flag = arg == "--assume-yes" || arg == "-y" ||
                      arg == "--no-download" || arg == "--include-headers" ||
                      arg == "--no-ignore" || arg == "--no-cache" ||
                      arg == "--entropy" || arg == "--override-memory-check";
    // Option values are data even when they spell an execution option, e.g.
    // score --prompt --backend or analyze --model --force-cpu.
    if (arg.starts_with('-') && !flag && i + 1 < argc) {
      append_argument(argv[++i]);
    }
  }
  return result + " --force-cpu";
}

std::string SmallerModelGuidance(std::optional<std::uint64_t> available) {
  std::string result =
      "Smaller models: --model-name qwen2.5-coder-3b-q6_k (~2.5 GB), then "
      "--model-name qwen2.5-coder-1.5b-q6_k (~1.3 GB); "
      "qwen2.5-coder-0.5b-q4_k_m (~0.4 GB) is the testing option.";
  if (available) {
    result += " Available memory: " + std::to_string(*available) + " bytes; ";
    if (*available > 3'000'000'000ULL)
      result += "3B may fit";
    else if (*available > 1'560'000'000ULL)
      result += "1.5B may fit";
    else if (*available > 480'000'000ULL)
      result += "only the testing model may fit";
    else
      result += "none is estimated to fit";
    result += " (weights plus 20% headroom; context also needs memory).";
  }
  return result;
}

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
  if (requested == BackendKind::kCpu) {
    return BackendKind::kCpu;
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

bool DeviceOutputGuaranteed(BackendKind backend, std::int32_t gpu_layers,
                            bool metal_backend) {
  // Cache identity is selected before model weights are loaded, so a partial
  // offload cannot prove that the final logits will remain on the accelerator.
  // Full offload is the only model-independent placement guarantee.
  return gpu_layers == -1 && (backend != BackendKind::kCpu || metal_backend);
}

}  // namespace llmcc
