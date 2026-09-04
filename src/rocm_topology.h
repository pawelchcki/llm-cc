#ifndef LLM_CC_ROCM_TOPOLOGY_H_
#define LLM_CC_ROCM_TOPOLOGY_H_

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace llmcc {

struct AmdGpuDevice {
  std::string architecture;
};

struct RocmTopology {
  std::vector<AmdGpuDevice> devices;
  std::optional<std::string> visible_devices;
  bool has_supported_device = false;
  bool has_unsupported_device = false;
};

// Reads KFD nodes in numeric node order. A missing KFD topology is a definitive
// empty inventory; malformed or inaccessible topology returns nullopt so
// callers can preserve ROCm's existing device discovery behavior.
std::optional<std::vector<AmdGpuDevice>> ReadAmdGpuDevices(
    const std::filesystem::path& nodes_directory);

// Returns GPU-agent ordinals only when supported and unsupported devices are
// both present. The input order is the GPU-only KFD order used by HIP.
std::optional<std::string> SelectRocmVisibleDevices(
    std::span<const AmdGpuDevice> devices,
    std::span<const std::string_view> supported_architectures);

std::optional<RocmTopology> InspectRocmTopology(
    const std::filesystem::path& nodes_directory,
    std::span<const std::string_view> supported_architectures);

// Inspects the system topology and applies the generated build target list.
// Existing ROCR_VISIBLE_DEVICES or HIP_VISIBLE_DEVICES values always win.
// This function never throws.
std::optional<RocmTopology> ConfigureRocmVisibility(
    const std::filesystem::path& nodes_directory =
        "/sys/class/kfd/kfd/topology/nodes") noexcept;

std::string RocmUnsupportedSystemMessage(const RocmTopology& topology);

}  // namespace llmcc

#endif  // LLM_CC_ROCM_TOPOLOGY_H_
