#include "src/rocm_topology.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include "generated/rocm_gpu_targets.h"

namespace llmcc {
namespace {

struct KfdNode {
  std::uint64_t number;
  std::filesystem::path path;
};

bool ParseUnsigned(std::string_view text, std::uint64_t& value) {
  if (text.empty()) {
    return false;
  }
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

std::optional<std::string> GfxArchitecture(std::uint64_t version) {
  const std::uint64_t major = version / 10000;
  const std::uint64_t minor = (version / 100) % 100;
  const std::uint64_t step = version % 100;
  if (major == 0 || minor > 15 || step > 15) {
    return std::nullopt;
  }
  constexpr char kHex[] = "0123456789abcdef";
  return "gfx" + std::to_string(major) + kHex[minor] + kHex[step];
}

std::optional<AmdGpuDevice> ReadGpuProperties(const std::filesystem::path& path,
                                              bool& is_cpu) {
  std::ifstream input(path);
  if (!input) {
    return std::nullopt;
  }
  std::optional<std::uint64_t> gfx_target_version;
  std::optional<std::uint64_t> simd_count;
  std::string line;
  while (std::getline(input, line)) {
    std::istringstream fields(line);
    std::string name;
    std::string value_text;
    if (!(fields >> name >> value_text)) {
      return std::nullopt;
    }
    if (name != "gfx_target_version" && name != "simd_count") {
      continue;
    }
    std::string trailing;
    std::uint64_t value = 0;
    if ((fields >> trailing) || !ParseUnsigned(value_text, value)) {
      return std::nullopt;
    }
    std::optional<std::uint64_t>& destination =
        name == "gfx_target_version" ? gfx_target_version : simd_count;
    if (destination.has_value()) {
      return std::nullopt;
    }
    destination = value;
  }
  if (!input.eof() || !gfx_target_version.has_value() ||
      !simd_count.has_value()) {
    return std::nullopt;
  }
  is_cpu = *gfx_target_version == 0 || *simd_count == 0;
  if (is_cpu) {
    return AmdGpuDevice{};
  }
  const std::optional<std::string> architecture =
      GfxArchitecture(*gfx_target_version);
  if (!architecture.has_value()) {
    return std::nullopt;
  }
  return AmdGpuDevice{.architecture = *architecture};
}

bool IsSupported(std::string_view architecture,
                 std::span<const std::string_view> supported_architectures) {
  return std::ranges::find(supported_architectures, architecture) !=
         supported_architectures.end();
}

std::string JoinArchitectures(std::span<const std::string_view> architectures) {
  std::string result;
  for (std::string_view architecture : architectures) {
    if (!result.empty()) {
      result += ", ";
    }
    result += architecture;
  }
  return result;
}

}  // namespace

std::optional<std::vector<AmdGpuDevice>> ReadAmdGpuDevices(
    const std::filesystem::path& nodes_directory) {
  std::error_code error;
  std::filesystem::directory_iterator iterator(nodes_directory, error);
  if (error) {
    return std::nullopt;
  }
  std::vector<KfdNode> nodes;
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    const std::filesystem::directory_entry entry = *iterator;
    if (!entry.is_directory(error) || error) {
      return std::nullopt;
    }
    std::uint64_t number = 0;
    const std::string filename = entry.path().filename().string();
    if (!ParseUnsigned(filename, number)) {
      return std::nullopt;
    }
    nodes.push_back({.number = number, .path = entry.path()});
    iterator.increment(error);
    if (error) {
      return std::nullopt;
    }
  }
  std::ranges::sort(nodes, {}, &KfdNode::number);

  std::vector<AmdGpuDevice> devices;
  for (const KfdNode& node : nodes) {
    bool is_cpu = false;
    std::optional<AmdGpuDevice> device =
        ReadGpuProperties(node.path / "properties", is_cpu);
    if (!device.has_value()) {
      return std::nullopt;
    }
    if (!is_cpu) {
      devices.push_back(std::move(*device));
    }
  }
  return devices;
}

std::optional<std::string> SelectRocmVisibleDevices(
    std::span<const AmdGpuDevice> devices,
    std::span<const std::string_view> supported_architectures) {
  std::string selected;
  bool has_supported = false;
  bool has_unsupported = false;
  for (std::size_t index = 0; index < devices.size(); ++index) {
    if (IsSupported(devices[index].architecture, supported_architectures)) {
      if (!selected.empty()) {
        selected += ',';
      }
      selected += std::to_string(index);
      has_supported = true;
    } else {
      has_unsupported = true;
    }
  }
  if (has_supported && has_unsupported) {
    return selected;
  }
  return std::nullopt;
}

std::optional<RocmTopology> InspectRocmTopology(
    const std::filesystem::path& nodes_directory,
    std::span<const std::string_view> supported_architectures) {
  std::optional<std::vector<AmdGpuDevice>> devices =
      ReadAmdGpuDevices(nodes_directory);
  if (!devices.has_value()) {
    return std::nullopt;
  }
  const bool has_unsupported =
      std::ranges::any_of(*devices, [&](const AmdGpuDevice& device) {
        return !IsSupported(device.architecture, supported_architectures);
      });
  std::optional<std::string> visible_devices =
      SelectRocmVisibleDevices(*devices, supported_architectures);
  return RocmTopology{
      .devices = std::move(*devices),
      .visible_devices = std::move(visible_devices),
      .has_unsupported_device = has_unsupported,
  };
}

std::optional<RocmTopology> ConfigureRocmVisibility(
    const std::filesystem::path& nodes_directory) noexcept {
  try {
    std::optional<RocmTopology> topology =
        InspectRocmTopology(nodes_directory, kRocmGpuTargets);
    if (!topology.has_value() || !topology->visible_devices.has_value() ||
        std::getenv("ROCR_VISIBLE_DEVICES") != nullptr ||
        std::getenv("HIP_VISIBLE_DEVICES") != nullptr) {
      return topology;
    }
#if defined(__linux__) || defined(__APPLE__)
    static_cast<void>(
        setenv("ROCR_VISIBLE_DEVICES", topology->visible_devices->c_str(), 0));
#endif
    return topology;
  } catch (...) {
    return std::nullopt;
  }
}

std::string RocmUnsupportedSystemMessage(const RocmTopology& topology) {
  std::vector<std::string_view> detected;
  detected.reserve(topology.devices.size());
  for (const AmdGpuDevice& device : topology.devices) {
    detected.push_back(device.architecture);
  }
  return "the ROCm backend does not support this system; detected AMD "
         "devices: " +
         JoinArchitectures(detected) + "; this build supports " +
         JoinArchitectures(kRocmGpuTargets);
}

}  // namespace llmcc
