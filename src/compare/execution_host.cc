#include "src/compare/execution_host.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef __linux__
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "src/cache_io.h"
#include "src/compare/identity.h"
#include "src/compare/json_util.h"
#include "src/rocm_topology.h"
#include "src/sha256.h"

namespace llmcc::compare {
namespace {

using nlohmann::json;

[[noreturn]] void Refuse(const std::string& message) {
  throw std::runtime_error(message);
}

bool ValidResourceId(const std::string& value) {
  static const std::regex kPattern("[a-z0-9][a-z0-9_-]{0,63}");
  return std::regex_match(value, kPattern);
}

#ifdef __linux__

std::uint64_t ParseHex(const std::string& text) {
  std::size_t used = 0;
  const std::uint64_t value = std::stoull(text, &used, 16);
  if (used != text.size()) {
    throw std::invalid_argument("not hexadecimal");
  }
  return value;
}

std::string ReadTrimmed(const std::filesystem::path& path) {
  std::string text = ReadFileBytes(path);
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
    text.pop_back();
  }
  return text;
}

std::string FileSha256(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  Sha256 hash;
  std::array<char, 1 << 16> buffer{};
  while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
    hash.Update(std::span<const char>(
        buffer.data(), static_cast<std::size_t>(input.gcount())));
  }
  if (input.bad()) {
    Refuse("cannot read runtime file " + path.string());
  }
  const auto digest = hash.Finish();
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string hex;
  for (const unsigned char byte : digest) {
    hex.push_back(kDigits[byte >> 4U]);
    hex.push_back(kDigits[byte & 0xFU]);
  }
  return hex;
}

void RequireReadWriteDevice(const std::filesystem::path& path) {
  struct stat details{};
  if (stat(path.c_str(), &details) != 0 || !S_ISCHR(details.st_mode) ||
      access(path.c_str(), R_OK | W_OK) != 0) {
    Refuse("GPU device is not readable and writable: " + path.string());
  }
}

void ValidateLockDirectory(const std::filesystem::path& directory) {
  std::error_code error;
  const std::filesystem::path canonical =
      std::filesystem::canonical(directory, error);
  if (error || canonical != directory) {
    Refuse("GPU lock directory must exist and not traverse symbolic links");
  }
  for (std::filesystem::path current = canonical;;
       current = current.parent_path()) {
    struct stat details{};
    if (stat(current.c_str(), &details) != 0 || details.st_uid != 0 ||
        (details.st_mode & 0022U) != 0) {
      Refuse(
          "GPU lock directory and its ancestors must be root-owned and not "
          "group or world writable");
    }
    if (current == current.root_path()) {
      break;
    }
  }
}

#endif

}  // namespace

ExecutionHost ParseExecutionHost(const json& host) {
  static const std::regex kPci("[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\\.[0-7]");
  static const std::regex kArchitecture("gfx[0-9a-f]+");
  if (!host.is_object() || host.value("gpu_vendor", json()) != "amd" ||
      !host.value("gpu_pci_address", json()).is_string() ||
      !std::regex_match(host["gpu_pci_address"].get<std::string>(), kPci) ||
      !host.value("gpu_arch", json()).is_string() ||
      !std::regex_match(host["gpu_arch"].get<std::string>(), kArchitecture) ||
      !host.value("gpu_vram_bytes_min", json()).is_number_unsigned() ||
      host["gpu_vram_bytes_min"].get<std::uint64_t>() == 0 ||
      !host.value("resource_id", json()).is_string() ||
      !ValidResourceId(host["resource_id"].get<std::string>()) ||
      !host.value("runtime_files", json()).is_object() ||
      host["runtime_files"].empty()) {
    throw std::invalid_argument(
        "execution host needs an AMD gpu_pci_address, gpu_arch, "
        "gpu_vram_bytes_min, resource_id and checksummed runtime_files");
  }
  ExecutionHost result{.pci_address = host["gpu_pci_address"],
                       .architecture = host["gpu_arch"],
                       .vram_bytes_min = host["gpu_vram_bytes_min"],
                       .resource_id = host["resource_id"]};
  for (const auto& [path, checksum] : host["runtime_files"].items()) {
    const std::filesystem::path file = std::filesystem::u8path(path);
    if (!file.is_absolute() || !checksum.is_string() ||
        !IsHexDigest(checksum.get_ref<const std::string&>(), 64)) {
      throw std::invalid_argument(
          "runtime_files must map absolute paths to SHA-256 digests");
    }
    result.runtime_files[path] = checksum.get<std::string>();
  }
  return result;
}

std::vector<std::pair<std::string, std::string>> VerifyExecutionHost(
    const ExecutionHost& host, const HostPaths& paths) {
#ifdef __linux__
  for (const auto& [file, checksum] : host.runtime_files) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error) ||
        FileSha256(file) != checksum) {
      Refuse("bare-host runtime checksum mismatch: " + file);
    }
  }
  const std::filesystem::path device =
      paths.sysfs / "bus/pci/devices" / host.pci_address;
  std::uint64_t vendor = 0;
  try {
    std::string text = ReadTrimmed(device / "vendor");
    if (text.starts_with("0x") || text.starts_with("0X")) {
      text.erase(0, 2);
    }
    vendor = ParseHex(text);
  } catch (const std::exception&) {
    Refuse("cannot read the PCI vendor of " + host.pci_address);
  }
  if (vendor != 0x1002) {
    Refuse("selected PCI device is not an AMD GPU");
  }
  const std::uint64_t domain = ParseHex(host.pci_address.substr(0, 4));
  const std::uint64_t location =
      (ParseHex(host.pci_address.substr(5, 2)) << 8U) |
      (ParseHex(host.pci_address.substr(8, 2)) << 3U) |
      ParseHex(host.pci_address.substr(11, 1));
  const auto devices =
      ReadAmdGpuDevices(paths.sysfs / "class/kfd/kfd/topology/nodes");
  if (!devices.has_value()) {
    Refuse("cannot read the KFD topology");
  }
  std::vector<const AmdGpuDevice*> matches;
  for (const AmdGpuDevice& candidate : *devices) {
    if (candidate.domain == domain && candidate.location_id == location &&
        (candidate.vendor_id == 0 || candidate.vendor_id == 0x1002)) {
      matches.push_back(&candidate);
    }
  }
  if (matches.size() != 1) {
    Refuse("selected GPU must match exactly one accessible KFD node");
  }
  const AmdGpuDevice& gpu = *matches.front();
  if (gpu.architecture != host.architecture) {
    Refuse("selected GPU architecture " + gpu.architecture +
           " differs from the execution host's " + host.architecture);
  }
  if (gpu.vram_bytes < host.vram_bytes_min) {
    Refuse("selected GPU has insufficient VRAM");
  }
  if (gpu.unique_id == 0) {
    Refuse("selected GPU has no usable ROCm UUID");
  }
  if (!gpu.drm_render_minor.has_value()) {
    Refuse("selected GPU has no DRM render node");
  }
  RequireReadWriteDevice(paths.devices / "kfd");
  RequireReadWriteDevice(paths.devices / "dri" /
                         ("renderD" + std::to_string(*gpu.drm_render_minor)));
  // ROCr UUIDs encode the KFD unique_id as 16 hexadecimal digits. Restricting
  // ROCr first leaves HIP exactly one device, ordinal zero.
  std::array<char, 32> uuid{};
  std::snprintf(uuid.data(), uuid.size(), "GPU-%016llx",  // NOLINT
                static_cast<unsigned long long>(gpu.unique_id));
  return {{"ROCR_VISIBLE_DEVICES", uuid.data()},
          {"HIP_VISIBLE_DEVICES", "0"},
          {"CUDA_VISIBLE_DEVICES", "0"}};
#else
  static_cast<void>(host);
  static_cast<void>(paths);
  Refuse("bare-host execution is supported on Linux only");
#endif
}

std::unique_ptr<cache_io::FileLock> AcquireGpuLease(
    const ExecutionHost& host, const HostPaths& paths,
    std::chrono::steady_clock::time_point deadline) {
#ifdef __linux__
  if (paths.require_root_owned_locks) {
    ValidateLockDirectory(paths.locks);
  }
  const std::filesystem::path lock = paths.locks / (host.resource_id + ".lock");
  std::error_code error;
  const auto status = std::filesystem::symlink_status(lock, error);
  if (error || status.type() != std::filesystem::file_type::regular) {
    Refuse("preprovision the shared GPU lock " + lock.string());
  }
  try {
    return std::make_unique<cache_io::FileLock>(
        lock,
        cache_io::FileLockOptions{.existing = true, .deadline = deadline});
  } catch (const std::exception& failure) {
    Refuse(std::string("cannot hold the shared GPU lock: ") + failure.what());
  }
#else
  static_cast<void>(host);
  static_cast<void>(paths);
  static_cast<void>(deadline);
  Refuse("bare-host execution is supported on Linux only");
#endif
}

}  // namespace llmcc::compare
