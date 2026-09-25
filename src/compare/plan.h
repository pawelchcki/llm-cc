#ifndef LLM_CC_COMPARE_PLAN_H_
#define LLM_CC_COMPARE_PLAN_H_

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "src/analysis_totals.h"

namespace llmcc::compare {

// At most this many workers score one plan's misses.
inline constexpr int kMaxWorkers = 4;

// A stored result: the inputs totals need, bound to one file, language and
// fingerprint.
//   {key, blob_id, language, fingerprint, llm_cc, token_count,
//    high_entropy_tokens, entropy_sum, total_branch, total_comp_level}
nlohmann::json ResultRecord(const nlohmann::json& item,
                            const std::string& fingerprint,
                            const FileTotals& totals);

// A result that belongs to `item` under `fingerprint` and has usable totals.
bool ValidResult(const nlohmann::json& result, const nlohmann::json& item,
                 const std::string& fingerprint);

struct WorkerAssignment {
  int worker_id = 0;
  std::vector<std::string> keys;
  std::uint64_t bytes = 0;
};

// Largest files first, each to the least-loaded worker (then the lowest id),
// over at most `max_workers` workers. `items` are plan items; the result does
// not depend on their order.
std::vector<WorkerAssignment> Partition(std::vector<nlohmann::json> items,
                                        int max_workers);
nlohmann::json AssignmentsJson(const std::vector<WorkerAssignment>& workers);

// Rejects a plan whose fingerprint, items, inventories, cached hits or
// worker assignments do not agree with each other. Throws
// std::invalid_argument naming the first inconsistency.
void ValidatePlan(const nlohmann::json& plan);

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_PLAN_H_
