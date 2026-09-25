// Bare-host GPU checks against a fake sysfs tree, ported from the Python
// worker tests.
#include "src/compare/execution_host.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "src/sha256.h"
#include "src/test_util.h"

namespace {

namespace fs = std::filesystem;
using llmcc::test::Expect;
using llmcc::test::ExpectEq;
using nlohmann::json;

void Write(const fs::path& path, const std::string& value) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << value;
}

struct FakeHost {
  fs::path root;
  fs::path runtime;
  llmcc::compare::HostPaths paths;

  explicit FakeHost(fs::path directory) : root(std::move(directory)) {
    fs::remove_all(root);
    runtime = root / "opt/rocm/libamdhip64.so";
    Write(runtime, "hip runtime");
    Write(root / "sys/bus/pci/devices/0000:03:00.0/vendor", "0x1002\n");
    Node("1", 768, 2408463410737365011ULL, 110000);
    Write(root / "sys/class/kfd/kfd/topology/nodes/0/properties",
          "cpu_cores_count 16\nsimd_count 0\ngfx_target_version 0\n");
    fs::create_directories(root / "dev/dri");
    fs::create_symlink("/dev/null", root / "dev/kfd");
    fs::create_symlink("/dev/null", root / "dev/dri/renderD128");
    fs::create_directories(root / "locks");
    Write(root / "locks/radeon-0.lock", "");
    paths = {.sysfs = root / "sys",
             .devices = root / "dev",
             .locks = root / "locks",
             .require_root_owned_locks = false};
  }

  void Node(const std::string& number, std::uint64_t location,
            std::uint64_t unique_id, std::uint64_t version) const {
    const fs::path node = root / "sys/class/kfd/kfd/topology/nodes" / number;
    Write(node / "properties",
          "simd_count 96\ngfx_target_version " + std::to_string(version) +
              "\nvendor_id 4098\ndomain 0\nlocation_id " +
              std::to_string(location) + "\nunique_id " +
              std::to_string(unique_id) + "\ndrm_render_minor 128\n");
    Write(node / "mem_banks/0/properties",
          "heap_type 1\nsize_in_bytes 25753026560\n");
  }

  [[nodiscard]] json Host() const {
    return {{"gpu_vendor", "amd"},
            {"gpu_pci_address", "0000:03:00.0"},
            {"gpu_arch", "gfx1100"},
            {"gpu_vram_bytes_min", 24000000000ULL},
            {"resource_id", "radeon-0"},
            {"runtime_files",
             {{runtime.string(), llmcc::Sha256Hex("hip runtime")}}}};
  }
};

std::string Refusal(const llmcc::compare::ExecutionHost& host,
                    const llmcc::compare::HostPaths& paths) {
  try {
    static_cast<void>(llmcc::compare::VerifyExecutionHost(host, paths));
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return {};
}

bool Contains(const std::string& text, const std::string& fragment) {
  return text.find(fragment) != std::string::npos;
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  const char* temporary = std::getenv("TEST_TMPDIR");
  Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const fs::path root = fs::path(temporary) / "host";

  {
    FakeHost fake(root);
    const auto host = llmcc::compare::ParseExecutionHost(fake.Host());
    const auto environment =
        llmcc::compare::VerifyExecutionHost(host, fake.paths);
    const std::map<std::string, std::string> variables(environment.begin(),
                                                       environment.end());
    ExpectEq(variables.at("ROCR_VISIBLE_DEVICES"),
             std::string("GPU-216c94b62386c013"),
             "ROCm sees exactly the verified GPU by UUID");
    ExpectEq(variables.at("HIP_VISIBLE_DEVICES"), std::string("0"),
             "HIP sees one device");

    // The lock is the preprovisioned file, held exclusively until released.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    auto lease = llmcc::compare::AcquireGpuLease(host, fake.paths, deadline);
    try {
      static_cast<void>(
          llmcc::compare::AcquireGpuLease(host, fake.paths, deadline));
      Expect(false, "a held GPU lock is exclusive");
    } catch (const std::runtime_error& error) {
      Expect(Contains(error.what(), "timed out"),
             "waiting for a held GPU stops at the deadline");
    }
    lease.reset();
    static_cast<void>(llmcc::compare::AcquireGpuLease(
        host, fake.paths,
        std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    llmcc::compare::HostPaths strict = fake.paths;
    strict.require_root_owned_locks = true;
    Expect(Contains(
               [&]() -> std::string {
                 try {
                   static_cast<void>(llmcc::compare::AcquireGpuLease(
                       host, strict, std::chrono::steady_clock::now()));
                 } catch (const std::runtime_error& error) {
                   return error.what();
                 }
                 return {};
               }(),
               "root-owned"),
           "lock directories writable by others are refused");
    fs::remove(root / "locks/radeon-0.lock");
    try {
      static_cast<void>(llmcc::compare::AcquireGpuLease(
          host, fake.paths, std::chrono::steady_clock::now()));
      Expect(false, "a missing lock is never created");
    } catch (const std::runtime_error& error) {
      Expect(Contains(error.what(), "preprovision"),
             "a missing lock names the provisioning step");
    }
    Expect(!fs::exists(root / "locks/radeon-0.lock"),
           "the worker never creates the lock");
  }

  // Each mismatch fails closed before any inference.
  const std::vector<
      std::pair<std::string, std::function<void(FakeHost&, json&)>>>
      refusals = {
          {"runtime checksum mismatch",
           [](FakeHost& fake, json&) { Write(fake.runtime, "replaced"); }},
          {"not an AMD GPU",
           [](FakeHost& fake, json&) {
             Write(fake.root / "sys/bus/pci/devices/0000:03:00.0/vendor",
                   "0x10de\n");
           }},
          {"architecture",
           [](FakeHost&, json& host) { host["gpu_arch"] = "gfx1030"; }},
          {"insufficient VRAM",
           [](FakeHost&, json& host) {
             host["gpu_vram_bytes_min"] = 32000000000ULL;
           }},
          {"exactly one",
           [](FakeHost& fake, json&) { fake.Node("2", 768, 5, 110000); }},
          {"exactly one",
           [](FakeHost&, json& host) {
             host["gpu_pci_address"] = "0000:04:00.0";
           }},
          {"ROCm UUID",
           [](FakeHost& fake, json&) { fake.Node("1", 768, 0, 110000); }},
          {"readable and writable",
           [](FakeHost& fake, json&) { fs::remove(fake.root / "dev/kfd"); }},
      };
  for (const auto& [message, mutate] : refusals) {
    FakeHost fake(root);
    json host = fake.Host();
    mutate(fake, host);
    if (host["gpu_pci_address"] != "0000:03:00.0") {
      Write(fake.root / "sys/bus/pci/devices" /
                host["gpu_pci_address"].get<std::string>() / "vendor",
            "0x1002\n");
    }
    const std::string refusal =
        Refusal(llmcc::compare::ParseExecutionHost(host), fake.paths);
    std::string description = "a mismatch is refused: " + message;
    description += "; got: " + refusal;
    Expect(Contains(refusal, message), description);
  }

  // Incomplete identities are rejected up front.
  FakeHost fake(root);
  for (const auto& [field, value] : std::vector<std::pair<std::string, json>>{
           {"gpu_vendor", "nvidia"},
           {"gpu_pci_address", "03:00.0"},
           {"gpu_arch", "sm_86"},
           {"gpu_vram_bytes_min", 0},
           {"resource_id", "../escape"},
           {"runtime_files", json::object()},
           {"runtime_files", {{"relative/lib.so", std::string(64, 'a')}}}}) {
    json host = fake.Host();
    host[field] = value;
    try {
      static_cast<void>(llmcc::compare::ParseExecutionHost(host));
      Expect(false, "an incomplete host is rejected: " + field);
    } catch (const std::invalid_argument&) {  // NOLINT(bugprone-empty-catch)
    }
  }
  return 0;
}
