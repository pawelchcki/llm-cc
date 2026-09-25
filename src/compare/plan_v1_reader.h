#ifndef LLM_CC_COMPARE_PLAN_V1_READER_H_
#define LLM_CC_COMPARE_PLAN_V1_READER_H_

#include <nlohmann/json.hpp>
#include <string>

namespace llmcc::compare::v1 {

inline constexpr int kSchemaVersion = 1;

// The fingerprint a version 1 plan derives from its profile.
std::string Fingerprint(const nlohmann::json& profile);

// Rejects a plan whose items, inventories, cached hits or worker assignments
// do not agree with each other and with its profile. Throws
// std::invalid_argument naming the first inconsistency.
void ValidatePlan(const nlohmann::json& plan);

// A result that belongs to `item` under `fingerprint` and has a usable score.
bool ValidResult(const nlohmann::json& result, const nlohmann::json& item,
                 const std::string& fingerprint);

}  // namespace llmcc::compare::v1

#endif  // LLM_CC_COMPARE_PLAN_V1_READER_H_
