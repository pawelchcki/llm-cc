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

// The schema version every configuration/totals event reports; comparison
// fingerprints include it, so a change invalidates their cached results.
inline constexpr int kAnalysisVersion = 3;

std::vector<EntropyRecord> ParseEntropyJsonl(std::string_view input);
std::vector<Token> AlignTokens(std::string_view source,
                               std::span<const EntropyRecord> records);
// Removes the SentencePiece dummy-prefix space from the first record so the
// records concatenate to the source exactly as the entropy cache expects, and
// reports whether it did. Records keep their positions: a first piece that is
// only the prefix is left alone, because dropping it would change the record
// count and with it the metadata a cache hit reconstructs.
bool NormalizeDummyPrefix(std::string_view source,
                          std::vector<EntropyRecord>& records);
void MapAnalysisOffsets(Analysis& analysis, const OffsetMap& map);
nlohmann::json AnalysisJson(const Analysis& analysis);
std::string PrettyAnalysisJson(const Analysis& analysis);

}  // namespace llmcc

#endif  // LLM_CC_JSONL_H_
