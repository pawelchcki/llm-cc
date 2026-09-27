#ifndef LLM_CC_ROCM_TOPOLOGY_H_
#define LLM_CC_ROCM_TOPOLOGY_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace llmcc {

struct AmdGpuDevice {
  std::string architecture;
  // KFD identity, when the node's properties report it: the PCI vendor, the
  // PCI domain and bus/device/function (bus << 8 | device << 3 | function),
  // the ROCr UUID and the DRM render node minor.
  std::uint64_t vendor_id = 0;
  std::uint64_t domain = 0;
  std::uint64_t location_id = 0;
  std::uint64_t unique_id = 0;
  std::optional<std::uint64_t> drm_render_minor;
  // Local (public and private framebuffer) memory over all memory banks.
  std::uint64_t vram_bytes = 0;
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

// Verifies that the current process can open the KFD device for both reading
// and writing. A visible sysfs topology alone does not imply container access.
bool RocmDeviceAccessible(
    const std::filesystem::path& device_path = "/dev/kfd") noexcept;

std::string RocmUnsupportedSystemMessage(const RocmTopology& topology);

}  // namespace llmcc

#endif  // LLM_CC_ROCM_TOPOLOGY_H_
