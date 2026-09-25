#ifndef LLM_CC_COMPARE_EXECUTION_HOST_H_
#define LLM_CC_COMPARE_EXECUTION_HOST_H_

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "src/cache_io.h"

namespace llmcc::compare {

// A bare-metal host's promise about the GPU a worker must use, independent
// of any runner label: the AMD device at a PCI address, its architecture and
// minimum VRAM, the host-wide lock that serializes it, and checksums of the
// runtime files the scorer loads.
struct ExecutionHost {
  std::string pci_address;
  std::string architecture;
  std::uint64_t vram_bytes_min = 0;
  std::string resource_id;
  std::map<std::string, std::string> runtime_files;
};

// Parses {gpu_vendor: "amd", gpu_pci_address, gpu_arch, gpu_vram_bytes_min,
// resource_id, runtime_files}. Throws std::invalid_argument.
ExecutionHost ParseExecutionHost(const nlohmann::json& host);

struct HostPaths {
  std::filesystem::path sysfs = "/sys";
  std::filesystem::path devices = "/dev";
  std::filesystem::path locks = "/var/lib/llm-cc/locks";
  // Only root may create or replace the lock and its directories.
  bool require_root_owned_locks = true;
};

// Fails closed unless the runtime files, the PCI vendor, exactly one KFD
// node, its architecture, VRAM and ROCm UUID, and read-write device nodes
// all match. Returns the environment that restricts ROCm to that GPU.
std::vector<std::pair<std::string, std::string>> VerifyExecutionHost(
    const ExecutionHost& host, const HostPaths& paths = {});

// Holds the preprovisioned lock for the host's GPU; never creates it, since
// replacing the inode would defeat the lock.
std::unique_ptr<cache_io::FileLock> AcquireGpuLease(
    const ExecutionHost& host, const HostPaths& paths,
    std::chrono::steady_clock::time_point deadline);

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_EXECUTION_HOST_H_
