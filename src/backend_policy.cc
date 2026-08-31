#include <limits>
#include <stdexcept>
#include <string_view>

#include "src/backend.h"

namespace llmcc {

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

}  // namespace llmcc
