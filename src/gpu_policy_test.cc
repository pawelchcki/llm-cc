#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

#include "src/backend.h"
#include "src/test_util.h"

int main() {
  using llmcc::GpuPolicy;
  using llmcc::GpuSelectionOptions;
  using llmcc::SelectGpuDevice;
  using llmcc::test::Expect;
  using llmcc::test::ExpectEq;
  const std::array<llmcc::GpuDeviceInfo, 3> devices = {{{.runtime_index = 1,
                                                         .name = "CUDA0",
                                                         .description = "busy",
                                                         .free_bytes = 100,
                                                         .total_bytes = 1000},
                                                        {.runtime_index = 2,
                                                         .name = "CUDA1",
                                                         .description = "idle",
                                                         .free_bytes = 900,
                                                         .total_bytes = 1000},
                                                        {.runtime_index = 3,
                                                         .name = "CUDA2",
                                                         .description = "idle",
                                                         .free_bytes = 900,
                                                         .total_bytes = 1000}}};
  const auto refuses = [&](const GpuSelectionOptions& options, int layers,
                           std::uint64_t bytes, const std::string& message) {
    try {
      SelectGpuDevice(options, layers, bytes, devices);
      Expect(false, "selection must refuse " + message);
    } catch (const std::runtime_error& error) {
      Expect(std::string(error.what()).find(message) != std::string::npos,
             message);
    } catch (const std::invalid_argument& error) {
      Expect(std::string(error.what()).find(message) != std::string::npos,
             message);
    }
  };
  Expect(!SelectGpuDevice({}, -1, 500, devices),
         "default preserves multi-GPU placement");
  Expect(!SelectGpuDevice({}, 0, 500, {}), "CPU never selects a GPU");
  ExpectEq(SelectGpuDevice({.policy = GpuPolicy::kMostFree}, -1, 500, devices),
           std::optional<std::size_t>(1),
           "busy first GPU is skipped; ties use visible order");
  ExpectEq(SelectGpuDevice({.policy = GpuPolicy::kMostFree, .device = "CUDA2"},
                           -1, 500, devices),
           std::optional<std::size_t>(2),
           "explicit visible name overrides policy");
  refuses({.device = "CUDA0"}, -1, 500, "insufficient free memory");
  refuses({.device = "CUDA9"}, -1, 500, "current visibility");
  refuses({.policy = GpuPolicy::kMostFree}, -1, 1000,
          "insufficient free memory");
  refuses({.policy = GpuPolicy::kMostFree, .min_free_bytes = 901}, 4, 500,
          "insufficient free memory");
  refuses({.policy = GpuPolicy::kMostFree}, 0, 0, "CPU execution");
  refuses({.device = "CUDA0"}, 0, 0, "CPU execution");
  refuses({.min_free_bytes = 1}, -1, 0, "requires --device");
  ExpectEq(SelectGpuDevice({.policy = GpuPolicy::kMostFree}, 4, 1000, devices),
           std::optional<std::size_t>(1),
           "partial offload does not assume all weights reside on GPU");
  ExpectEq(llmcc::GpuMemoryRequirement(
               {}, -1, std::numeric_limits<std::uint64_t>::max()),
           std::numeric_limits<std::uint64_t>::max(),
           "memory estimate saturates");
  GpuSelectionOptions options;
  Expect(llmcc::ParseGpuSelectionOption(options, "--gpu-policy", "most-free"),
         "policy parses");
  Expect(llmcc::ParseGpuSelectionOption(options, "--device", "CUDA1"),
         "device parses");
  Expect(llmcc::ParseGpuSelectionOption(options, "--gpu-min-free", "800"),
         "floor parses");
  Expect(options.policy == GpuPolicy::kMostFree && options.device == "CUDA1" &&
             options.min_free_bytes == 800,
         "selection options retained");
  for (const char* invalid : {"-1", "1GB", "18446744073709551616"}) {
    try {
      llmcc::ParseGpuSelectionOption(options, "--gpu-min-free", invalid);
      Expect(false, "invalid floor rejected");
    } catch (const std::invalid_argument&) {
    }
  }
  try {
    SelectGpuDevice({.policy = GpuPolicy::kMostFree}, -1, 0, {});
    Expect(false, "empty inventory refused");
  } catch (const llmcc::GpuRecoverableError&) {
  }
}
