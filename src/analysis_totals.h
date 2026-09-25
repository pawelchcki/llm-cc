#ifndef LLM_CC_ANALYSIS_TOTALS_H_
#define LLM_CC_ANALYSIS_TOTALS_H_

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "src/core.h"

namespace llmcc {

// The per-file quantities that totals are built from; a stored comparison
// result carries exactly these.
struct FileTotals {
  double llm_cc = 0.0;
  std::uint64_t total_branch = 0;
  std::uint64_t total_comp_level = 0;
  std::uint64_t token_count = 0;
  std::uint64_t high_entropy_tokens = 0;
  double entropy_sum = 0.0;
};

FileTotals FileTotalsOf(const Analysis& analysis);

struct MetricTotals {
  std::uint64_t discovered = 0;
  std::uint64_t analyzed = 0;
  std::uint64_t failed = 0;
  double llm_cc = 0.0;
  std::uint64_t total_branch = 0;
  std::uint64_t total_comp_level = 0;
  std::uint64_t token_count = 0;
  std::uint64_t high_entropy_tokens = 0;
  double entropy_sum = 0.0;
  double lmcc = 0.0;
};

// Raw LM-CC and the hierarchy counts add up over every analyzed file; the
// token-normalized sums include only files with scored tokens.
void Accumulate(const FileTotals& file, MetricTotals& totals);
void Accumulate(const Analysis& analysis, MetricTotals& totals);

Metrics TotalMetrics(const MetricTotals& totals);

// The headline for `score_mode` (raw, lmcc, density or mean). Scores other
// than raw LM-CC are null without scored tokens.
nlohmann::json ScoreJson(const Metrics& metrics, std::string_view score_mode);
nlohmann::json TotalsMetricsJson(const MetricTotals& totals,
                                 std::string_view score_mode);

// Totals broken down by language and by rules category.
struct GroupTotals {
  std::map<std::string, MetricTotals> languages;
  std::map<std::string, MetricTotals> categories;
};

nlohmann::json GroupTotalsJson(
    const std::map<std::string, MetricTotals>& groups,
    std::string_view score_mode);
nlohmann::json TotalsJson(const MetricTotals& totals, const GroupTotals& groups,
                          bool fatal, std::string_view score_mode,
                          HierarchyMode hierarchy_mode);

}  // namespace llmcc

#endif  // LLM_CC_ANALYSIS_TOTALS_H_
