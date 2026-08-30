#ifndef RETHINK_CC_SCORING_H_
#define RETHINK_CC_SCORING_H_

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace rethink {

struct TokenScore {
  double probability;
  double log_probability;
};

TokenScore ScoreToken(std::span<const float> logits, std::size_t token_id);
std::string BytesToHex(std::string_view bytes);
std::string JsonEscapeBytes(std::string_view bytes);

}  // namespace rethink

#endif  // RETHINK_CC_SCORING_H_
