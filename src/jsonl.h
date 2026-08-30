#ifndef LLM_CC_JSONL_H_
#define LLM_CC_JSONL_H_

#include <cstddef>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/core.h"
#include "src/lang.h"

namespace llmcc {

struct EntropyRecord {
  std::size_t position;
  std::string bytes;
  std::optional<double> entropy;
};

std::vector<EntropyRecord> ParseEntropyJsonl(std::string_view input);
std::vector<Token> AlignTokens(std::string_view source,
                               std::span<const EntropyRecord> records);
void MapAnalysisOffsets(Analysis& analysis, std::span<const std::size_t> map);
nlohmann::json AnalysisJson(const Analysis& analysis);
std::string PrettyAnalysisJson(const Analysis& analysis);

}  // namespace llmcc

#endif  // LLM_CC_JSONL_H_
