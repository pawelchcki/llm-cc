#ifndef LLM_CC_COMPARE_REPORT_MODEL_H_
#define LLM_CC_COMPARE_REPORT_MODEL_H_

#include <cstddef>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace llmcc::compare {

// Report arithmetic keeps Python's number kinds: integer inputs give integer
// differences, and every ratio or percentage is a float.

// {base, head, absolute, percent}; undefined when either side is null, and
// the percentage is undefined for a zero base.
nlohmann::json Delta(const nlohmann::json& base, const nlohmann::json& head);

// Per-category and total coverage and scores for one inventory. A category
// or side has an LM-CC total only when every supported path was measured.
nlohmann::json Side(const nlohmann::json& inventory,
                    const nlohmann::json& results);

// Measured files by descending total LM-CC, then path, with overall and
// per-category ranks and whether a change touched them.
nlohmann::json Rankings(const nlohmann::json& inventory,
                        const nlohmann::json& results,
                        const std::set<std::string>& changed_paths);

// One row per change that has a measured score on either side.
nlohmann::json ChangedFiles(const nlohmann::json& changes,
                            const nlohmann::json& base_inventory,
                            const nlohmann::json& head_inventory,
                            const nlohmann::json& results,
                            const nlohmann::json& head_rankings);

struct ReportInputs {
  int schema_version = 1;
  nlohmann::json identity;
  nlohmann::json fingerprint;
  // Scorer identity copied into the report as-is.
  nlohmann::json header = nlohmann::json::object();
  nlohmann::json inventories;
  nlohmann::json changes;
  // Result records by key, from the cache and every valid worker.
  nlohmann::json results = nlohmann::json::object();
  nlohmann::json rules_source;
  nlohmann::json presentation;
  nlohmann::json cache_stats;
  std::vector<std::string> errors;
  // Planned keys without a result.
  std::size_t missing = 0;
};

// The complete report: sides, comparisons, changes, rankings, the leading
// regressions and improvements, and a status.
nlohmann::json BuildReport(const ReportInputs& inputs);

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_REPORT_MODEL_H_
