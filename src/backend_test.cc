#include "src/backend.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/test_util.h"

namespace {

template <typename Exception, typename Function>
bool ThrowsContaining(Function function, const std::string& needle) {
  try {
    function();
  } catch (const Exception& error) {
    return std::string(error.what()).find(needle) != std::string::npos;
  }
  return false;
}

}  // namespace

int main() {
  using llmcc::BackendDevice;
  using llmcc::BackendKind;
  using llmcc::SelectBackend;
  using llmcc::test::Expect;
  using llmcc::test::ExpectEq;

  ExpectEq(llmcc::ParseBackend("auto"), BackendKind::kAuto, "parse auto");
  ExpectEq(llmcc::ParseBackend("cpu"), BackendKind::kCpu, "parse cpu");
  ExpectEq(llmcc::ParseBackend("cuda"), BackendKind::kCuda, "parse cuda");
  ExpectEq(llmcc::ParseBackend("rocm"), BackendKind::kRocm, "parse rocm");
  Expect(ThrowsContaining<std::invalid_argument>(
             [] { static_cast<void>(llmcc::ParseBackend("metal")); },
             "auto, cpu, cuda, or rocm"),
         "invalid backend is explained");

  ExpectEq(SelectBackend(BackendKind::kAuto, 0, {}), BackendKind::kCpu,
           "auto without offload selects CPU");
  ExpectEq(SelectBackend(BackendKind::kCpu, 0, {}), BackendKind::kCpu,
           "explicit CPU without offload selects CPU");
  Expect(ThrowsContaining<std::invalid_argument>(
             [] { SelectBackend(BackendKind::kCpu, 1, {}); }, "--backend cpu"),
         "CPU offload is rejected");
  Expect(
      ThrowsContaining<std::runtime_error>(
          [] { SelectBackend(BackendKind::kCuda, 1, {}); }, "no usable CUDA"),
      "missing explicit CUDA is explained");
  Expect(
      ThrowsContaining<std::runtime_error>(
          [] { SelectBackend(BackendKind::kRocm, -1, {}); }, "no usable ROCm"),
      "missing explicit ROCm is explained");

  const std::vector<BackendDevice> devices = {
      {.backend = BackendKind::kCuda, .free_memory = 3},
      {.backend = BackendKind::kCuda, .free_memory = 5},
      {.backend = BackendKind::kRocm, .free_memory = 7},
  };
  ExpectEq(SelectBackend(BackendKind::kAuto, -1, devices), BackendKind::kCuda,
           "free VRAM is aggregated per family");

  const std::vector<BackendDevice> rocm_larger = {
      {.backend = BackendKind::kCuda, .free_memory = 8},
      {.backend = BackendKind::kRocm, .free_memory = 5},
      {.backend = BackendKind::kRocm, .free_memory = 5},
  };
  ExpectEq(SelectBackend(BackendKind::kAuto, 4, rocm_larger),
           BackendKind::kRocm, "larger aggregate ROCm memory wins");

  const std::vector<BackendDevice> tied = {
      {.backend = BackendKind::kCuda, .free_memory = 10},
      {.backend = BackendKind::kRocm, .free_memory = 10},
  };
  ExpectEq(SelectBackend(BackendKind::kAuto, 1, tied), BackendKind::kCuda,
           "CUDA wins ties");
  Expect(!llmcc::DeviceOutputGuaranteed(BackendKind::kCuda, 1, false),
         "partial offload does not guarantee device output");
  Expect(llmcc::DeviceOutputGuaranteed(BackendKind::kCuda, -1, false),
         "full CUDA offload guarantees device output");
  Expect(llmcc::DeviceOutputGuaranteed(BackendKind::kCpu, -1, true),
         "full Metal offload guarantees device output");
  Expect(!llmcc::DeviceOutputGuaranteed(BackendKind::kCpu, -1, false),
         "a CPU build cannot guarantee device output");
  return 0;
}
