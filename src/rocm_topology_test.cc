#include "src/rocm_topology.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "src/test_util.h"

namespace {

constexpr std::array<std::string_view, 3> kSupported = {"gfx1100", "gfx1101",
                                                        "gfx1102"};

void WriteNode(const std::filesystem::path& root, std::string_view number,
               std::string_view properties) {
  const std::filesystem::path directory = root / number;
  std::filesystem::create_directories(directory);
  std::ofstream output(directory / "properties");
  output << properties;
  if (!output) {
    throw std::runtime_error("could not write fake KFD properties");
  }
}

void ClearVisibility() {
#if defined(__linux__) || defined(__APPLE__)
  unsetenv("ROCR_VISIBLE_DEVICES");
  unsetenv("HIP_VISIBLE_DEVICES");
#endif
}

}  // namespace

int main() try {
  using llmcc::AmdGpuDevice;
  using llmcc::test::Expect;
  using llmcc::test::ExpectEq;
  namespace fs = std::filesystem;

  const char* temporary = std::getenv("TEST_TMPDIR");
  Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const fs::path root = fs::path(temporary) / "rocm-topology";
  std::error_code error;
  fs::remove_all(root, error);
  fs::create_directories(root);

  const fs::path mixed = root / "mixed";
  // Raw KFD node 0 is a CPU and must not consume a HIP GPU ordinal. Deliberate
  // creation order also verifies that raw node numbers are sorted numerically.
  WriteNode(mixed, "10", "simd_count 4\ngfx_target_version 100306\n");
  WriteNode(mixed, "0", "gfx_target_version 0\nsimd_count 0\n");
  WriteNode(mixed, "2", "simd_count 96\ngfx_target_version 110000\n");
  const auto devices = llmcc::ReadAmdGpuDevices(mixed);
  Expect(devices.has_value(), "mixed topology parses");
  ExpectEq(devices->size(), std::size_t{2}, "CPU node is ignored");
  ExpectEq((*devices)[0].architecture, std::string("gfx1100"),
           "supported GPU is first GPU-agent ordinal");
  ExpectEq((*devices)[1].architecture, std::string("gfx1036"),
           "unsupported GPU architecture is encoded like ROCm");
  ExpectEq(llmcc::SelectRocmVisibleDevices(*devices, kSupported),
           std::optional<std::string>("0"),
           "mixed topology selects supported GPU ordinal");
  const auto mixed_topology = llmcc::InspectRocmTopology(mixed, kSupported);
  Expect(mixed_topology.has_value(), "mixed topology inspection succeeds");
  ExpectEq(llmcc::RocmUnsupportedSystemMessage(*mixed_topology),
           std::string("the ROCm backend does not support this system; "
                       "detected AMD devices: gfx1100, gfx1036; this build "
                       "supports gfx1100, gfx1101, gfx1102"),
           "unsupported-system diagnostic names detected and built targets");

  const std::array all_supported = {AmdGpuDevice{"gfx1100"},
                                    AmdGpuDevice{"gfx1102"}};
  Expect(
      !llmcc::SelectRocmVisibleDevices(all_supported, kSupported).has_value(),
      "all-supported topology needs no visibility filter");
  const std::array none_supported = {AmdGpuDevice{"gfx1036"},
                                     AmdGpuDevice{"gfx900"}};
  Expect(
      !llmcc::SelectRocmVisibleDevices(none_supported, kSupported).has_value(),
      "unsupported-only topology needs no visibility filter");

  const fs::path all_supported_tree = root / "all-supported";
  WriteNode(all_supported_tree, "0",
            "simd_count 96\ngfx_target_version 110000\n");
  WriteNode(all_supported_tree, "1",
            "simd_count 60\ngfx_target_version 110002\n");
  ClearVisibility();
  Expect(llmcc::ConfigureRocmVisibility(all_supported_tree).has_value(),
         "all-supported topology parses");
  Expect(std::getenv("ROCR_VISIBLE_DEVICES") == nullptr,
         "all-supported topology does not set visibility");

  const fs::path none_supported_tree = root / "none-supported";
  WriteNode(none_supported_tree, "0",
            "simd_count 4\ngfx_target_version 100306\n");
  ClearVisibility();
  Expect(llmcc::ConfigureRocmVisibility(none_supported_tree).has_value(),
         "unsupported-only topology parses");
  Expect(std::getenv("ROCR_VISIBLE_DEVICES") == nullptr,
         "unsupported-only topology does not set visibility");

  const fs::path malformed = root / "malformed";
  WriteNode(malformed, "0", "simd_count nope\ngfx_target_version 110000\n");
  ClearVisibility();
  Expect(!llmcc::ConfigureRocmVisibility(malformed).has_value(),
         "malformed topology falls through");
  Expect(std::getenv("ROCR_VISIBLE_DEVICES") == nullptr,
         "malformed topology does not set visibility");

  ClearVisibility();
  const auto configured = llmcc::ConfigureRocmVisibility(mixed);
  Expect(configured.has_value(), "mixed topology configures successfully");
  ExpectEq(std::string(std::getenv("ROCR_VISIBLE_DEVICES")), std::string("0"),
           "mixed topology sets selected GPU ordinal");

#if defined(__linux__) || defined(__APPLE__)
  setenv("ROCR_VISIBLE_DEVICES", "7", 1);
#endif
  static_cast<void>(llmcc::ConfigureRocmVisibility(mixed));
  ExpectEq(std::string(std::getenv("ROCR_VISIBLE_DEVICES")), std::string("7"),
           "user-provided ROCR visibility is respected");

  ClearVisibility();
#if defined(__linux__) || defined(__APPLE__)
  setenv("HIP_VISIBLE_DEVICES", "5", 1);
#endif
  static_cast<void>(llmcc::ConfigureRocmVisibility(mixed));
  ExpectEq(std::string(std::getenv("HIP_VISIBLE_DEVICES")), std::string("5"),
           "user-provided HIP visibility is respected");
  Expect(std::getenv("ROCR_VISIBLE_DEVICES") == nullptr,
         "ROCR visibility is not set when HIP visibility is present");

  ClearVisibility();
  return 0;
} catch (const std::exception& exception) {
  std::cerr << "FAIL: unexpected exception: " << exception.what() << '\n';
  return EXIT_FAILURE;
}
