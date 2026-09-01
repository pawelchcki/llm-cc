#ifndef LLM_CC_INFERENCE_GUARD_H_
#define LLM_CC_INFERENCE_GUARD_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace llmcc {

class InferenceGuard {
 public:
  explicit InferenceGuard(std::string_view backend);
  ~InferenceGuard();
  InferenceGuard(const InferenceGuard&) = delete;
  InferenceGuard& operator=(const InferenceGuard&) = delete;

 private:
  int descriptor_ = -1;
};

std::optional<std::string> CodexSandboxGpuWarning(
    std::optional<std::uint64_t> gpu_available, const char* sandbox);

}  // namespace llmcc

#endif  // LLM_CC_INFERENCE_GUARD_H_
