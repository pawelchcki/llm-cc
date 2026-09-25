#ifndef LLM_CC_COMPARE_PREPARE_H_
#define LLM_CC_COMPARE_PREPARE_H_

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "src/compare/result_cache.h"
#include "src/compare/store.h"

namespace llmcc::compare {

inline constexpr std::uint64_t kDefaultMaxFileBytes = 65536;

struct PrepareOptions {
  std::filesystem::path repository = ".";
  std::string head;
  std::string target;
  // {repository, pipeline_id, target_branch?, pr_number?, started_at?}; the
  // commits are resolved here.
  nlohmann::json identity;
  std::filesystem::path output;
  // Null plans every file as a miss.
  Store* cache = nullptr;
  int cache_concurrency = 8;
  int refresh_days = 20;
  int expire_days = 30;
  int max_workers = 4;
  std::uint64_t max_file_bytes = kDefaultMaxFileBytes;
  std::optional<std::filesystem::path> default_rules;
  nlohmann::json presentation = nlohmann::json::object();
  // The identity components from ScorerJson, ModelJson and ScoringJson.
  nlohmann::json scorer;
  nlohmann::json model;
  nlohmann::json scoring;
  ResultCache::Clock now;
};

// Inventories the merge base and head, resolves languages and categories
// with the target commit's rules, looks up every unique result, writes the
// bytes of each miss to blobs/<blob_id>, partitions the misses over at most
// four workers, and writes plan.json last. Returns the plan.
nlohmann::json Prepare(const PrepareOptions& options);

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_PREPARE_H_
