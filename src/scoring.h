#ifndef RETHINK_CC_SCORING_H_
#define RETHINK_CC_SCORING_H_

#include <cstddef>
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

TokenScore ScoreToken(std::span<const float> logits, std::size_t token_id,
                      bool compute_entropy = false);
std::string BytesToHex(std::string_view bytes);
std::string JsonEscapeBytes(std::string_view bytes);

}  // namespace rethink

#endif  // RETHINK_CC_SCORING_H_
