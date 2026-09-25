#ifndef LLM_CC_COMPARE_AGGREGATE_H_
#define LLM_CC_COMPARE_AGGREGATE_H_

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace llmcc::compare {

// Validates a plan and its worker artifacts, merges cached and fresh
// results, and writes the report set to `output`. Any failure to read or
// validate the plan becomes a failure report instead. Worker paths are
// quoted in errors exactly as given. Returns the report.
nlohmann::json AggregatePlan(const std::filesystem::path& plan_path,
                             const std::vector<std::string>& worker_paths,
                             const std::filesystem::path& output);

// Writes the report set for a run that produced no comparison. `identity`
// may be null or partial; its fields are normalized.
nlohmann::json WriteFailureReport(const std::filesystem::path& output,
                                  const nlohmann::json& identity,
                                  const nlohmann::json& fingerprint,
                                  const std::vector<std::string>& errors);

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_AGGREGATE_H_
