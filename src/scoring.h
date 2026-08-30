#ifndef RETHINK_CC_SCORING_H_
#define RETHINK_CC_SCORING_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace rethink {

struct TokenScore {
  double probability;
  double log_probability;
  std::optional<double> entropy;
};

struct MemoryCheckResult {
  bool ok;
  std::string error;
};

TokenScore ScoreToken(std::span<const float> logits, std::size_t token_id,
                      bool compute_entropy = false);
std::string BytesToHex(std::string_view bytes);
std::string JsonEscapeBytes(std::string_view bytes);
MemoryCheckResult CheckMemory(std::uint64_t model_bytes,
                              std::uint64_t host_available_bytes,
                              std::optional<std::uint64_t> gpu_available_bytes,
                              std::int32_t gpu_layers,
                              bool override_check);

}  // namespace rethink

#endif  // RETHINK_CC_SCORING_H_
