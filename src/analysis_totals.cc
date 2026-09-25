#include "src/analysis_totals.h"

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "src/jsonl.h"

namespace llmcc {

FileTotals FileTotalsOf(const Analysis& analysis) {
  return {.llm_cc = analysis.llm_cc,
          .total_branch = analysis.total_branch,
          .total_comp_level = analysis.total_comp_level,
          .token_count = analysis.metrics.token_count,
          .high_entropy_tokens = analysis.metrics.high_entropy_tokens,
          .entropy_sum = analysis.metrics.entropy_sum};
}

void Accumulate(const FileTotals& file, MetricTotals& totals) {
  totals.llm_cc += file.llm_cc;
  totals.total_branch += file.total_branch;
  totals.total_comp_level += file.total_comp_level;
  if (file.token_count == 0) {
    return;
  }
  totals.token_count += file.token_count;
  totals.high_entropy_tokens += file.high_entropy_tokens;
  totals.entropy_sum += file.entropy_sum;
  // A file's raw LM-CC is its metrics' lmcc.
  totals.lmcc += file.llm_cc;
}

void Accumulate(const Analysis& analysis, MetricTotals& totals) {
  Accumulate(FileTotalsOf(analysis), totals);
}

Metrics TotalMetrics(const MetricTotals& totals) {
  // `lmcc` carries the raw headline over every analyzed file; the per-token
  // normalization keeps using only files with scored tokens.
  if (totals.token_count == 0) {
    return {.lmcc = totals.llm_cc};
  }
  const double tokens = static_cast<double>(totals.token_count);
  return {.token_count = totals.token_count,
          .high_entropy_tokens = totals.high_entropy_tokens,
          .entropy_sum = totals.entropy_sum,
          .lmcc = totals.llm_cc,
          .lmcc_per_token = totals.lmcc / tokens,
          .density = static_cast<double>(totals.high_entropy_tokens) / tokens,
          .mean_entropy = totals.entropy_sum / tokens};
}

nlohmann::json ScoreJson(const Metrics& metrics, std::string_view score_mode) {
  // Raw LM-CC is defined for every analyzed input, including unscored ones.
  if (score_mode == "raw") {
    return metrics.lmcc;
  }
  if (metrics.token_count == 0) {
    return nullptr;
  }
  if (score_mode == "density") {
    return metrics.density;
  }
  if (score_mode == "mean") {
    return metrics.mean_entropy;
  }
  return metrics.lmcc_per_token;
}

nlohmann::json TotalsMetricsJson(const MetricTotals& totals,
                                 std::string_view score_mode) {
  const auto metrics = TotalMetrics(totals);
  return {{"score", ScoreJson(metrics, score_mode)},
          {"mean_llm_cc_per_file",
           totals.analyzed == 0
               ? nlohmann::json()
               : nlohmann::json(totals.llm_cc /
                                static_cast<double>(totals.analyzed))},
          {"lmcc_per_token", ScoreJson(metrics, "lmcc")},
          {"density", ScoreJson(metrics, "density")},
          {"mean_entropy", ScoreJson(metrics, "mean")},
          {"token_count", totals.token_count},
          {"high_entropy_tokens", totals.high_entropy_tokens}};
}

nlohmann::json GroupTotalsJson(
    const std::map<std::string, MetricTotals>& groups,
    std::string_view score_mode) {
  nlohmann::json result = nlohmann::json::object();
  for (const auto& [name, value] : groups) {
    nlohmann::json item = TotalsMetricsJson(value, score_mode);
    item.update({{"discovered", value.discovered},
                 {"analyzed", value.analyzed},
                 {"failed", value.failed},
                 {"llm_cc", value.llm_cc},
                 {"total_branch", value.total_branch},
                 {"total_comp_level", value.total_comp_level}});
    result[name] = std::move(item);
  }
  return result;
}

nlohmann::json TotalsJson(const MetricTotals& totals, const GroupTotals& groups,
                          bool fatal, std::string_view score_mode,
                          HierarchyMode hierarchy_mode) {
  nlohmann::json result = TotalsMetricsJson(totals, score_mode);
  result.update({{"type", "totals"},
                 {"analysis_version", kAnalysisVersion},
                 {"hierarchy_mode", hierarchy_mode == HierarchyMode::kStructural
                                        ? "structural"
                                        : "reference"},
                 {"discovered", totals.discovered},
                 {"analyzed", totals.analyzed},
                 {"failed", totals.failed},
                 {"llm_cc", totals.llm_cc},
                 {"total_branch", totals.total_branch},
                 {"total_comp_level", totals.total_comp_level},
                 {"languages", GroupTotalsJson(groups.languages, score_mode)},
                 {"categories", GroupTotalsJson(groups.categories, score_mode)},
                 {"partial", fatal || totals.failed != 0 ||
                                 totals.analyzed != totals.discovered}});
  return result;
}

}  // namespace llmcc
