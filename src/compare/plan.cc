#include "src/compare/plan.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "src/compare/identity.h"
#include "src/compare/json_util.h"
#include "src/input_limits.h"
#include "src/lang.h"

namespace llmcc::compare {
namespace {

using nlohmann::json;

constexpr std::array<std::string_view, 10> kResultFields = {
    "key",
    "blob_id",
    "language",
    "fingerprint",
    "llm_cc",
    "token_count",
    "high_entropy_tokens",
    "entropy_sum",
    "total_branch",
    "total_comp_level"};

constexpr std::array<std::string_view, 8> kIdentityFields = {
    "repository", "pipeline_id",   "head_sha",  "target_sha",
    "base_sha",   "target_branch", "pr_number", "started_at"};

[[noreturn]] void Invalid(const std::string& message) {
  throw std::invalid_argument(message);
}

bool IsBlobId(const json& value) {
  return value.is_string() &&
         (IsHexDigest(value.get_ref<const std::string&>(), 40) ||
          IsHexDigest(value.get_ref<const std::string&>(), 64));
}

bool IsCanonicalLanguage(const json& value) {
  if (!value.is_string()) {
    return false;
  }
  try {
    const auto& name = value.get_ref<const std::string&>();
    return LanguageName(ParseLanguage(name)) == name;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

// A file has no more tokens, branches or nesting levels than bytes, and
// bounding each count keeps the report's signed sums far from overflowing.
bool IsCount(const json& value) {
  return value.is_number_unsigned() &&
         value.get<std::uint64_t>() <= kMaxSourceBytes;
}

bool IsFiniteNonNegative(const json& value) {
  return value.is_number() && std::isfinite(value.get<double>()) &&
         value.get<double>() >= 0;
}

const json& Field(const json& object, std::string_view name) {
  static const json kNull;
  if (!object.is_object()) {
    return kNull;
  }
  const auto found = object.find(name);
  return found == object.end() ? kNull : *found;
}

void ValidateIdentity(const json& identity) {
  if (!identity.is_object() || identity.size() != kIdentityFields.size()) {
    Invalid("plan identity must hold exactly the pipeline identity fields");
  }
  for (const std::string_view field : kIdentityFields) {
    if (!identity.contains(field)) {
      Invalid("plan identity lacks " + std::string(field));
    }
  }
  for (const char* field : {"repository", "pipeline_id"}) {
    if (!identity[field].is_string() ||
        identity[field].get_ref<const std::string&>().empty()) {
      Invalid(std::string("plan identity needs a ") + field);
    }
  }
  for (const char* field : {"head_sha", "target_sha", "base_sha"}) {
    const json& sha = identity[field];
    if (!sha.is_string() ||
        !(IsHexDigest(sha.get_ref<const std::string&>(), 40) ||
          IsHexDigest(sha.get_ref<const std::string&>(), 64))) {
      Invalid(std::string("plan identity ") + field + " must be a commit");
    }
  }
}

void ValidateItem(const std::string& key, const json& item,
                  const std::string& fingerprint) {
  if (!item.is_object() || item.size() != 4 || Field(item, "key") != key ||
      !IsBlobId(Field(item, "blob_id")) ||
      !IsCanonicalLanguage(Field(item, "language")) ||
      !Field(item, "size").is_number_unsigned()) {
    Invalid("plan item " + key + " is malformed");
  }
  if (key != ResultKey(item["blob_id"].get<std::string>(),
                       item["language"].get<std::string>(), fingerprint)) {
    Invalid("plan item key does not match its provenance");
  }
}

std::set<std::string> ValidateInventory(const json& inventory,
                                        const json& items) {
  if (!inventory.is_array()) {
    Invalid("plan inventory must be a list");
  }
  std::set<std::string> paths;
  std::set<std::string> referenced;
  for (const json& file : inventory) {
    if (!file.is_object() || file.size() != 8 ||
        !Field(file, "path").is_string()) {
      Invalid("inventory entry is malformed");
    }
    if (!paths.insert(file["path"].get<std::string>()).second) {
      Invalid("inventory has duplicate paths");
    }
    const json& category = Field(file, "category");
    if (category != "runtime" && category != "tests" && category != "tooling") {
      Invalid("inventory has an invalid category");
    }
    const json& language = Field(file, "language");
    const json& size = Field(file, "size");
    const json& blob_id = Field(file, "blob_id");
    if ((!language.is_null() && !IsCanonicalLanguage(language)) ||
        (!size.is_null() && !size.is_number_unsigned()) ||
        (!blob_id.is_null() && !IsBlobId(blob_id)) ||
        !Field(file, "scorable").is_boolean()) {
      Invalid("inventory entry " + file["path"].get<std::string>() +
              " is malformed");
    }
    const json& key = Field(file, "key");
    const json& reason = Field(file, "reason");
    if (file["scorable"].get<bool>()) {
      if (!key.is_string() || !items.contains(key.get<std::string>()) ||
          !reason.is_null()) {
        Invalid("inventory references an invalid scorable item");
      }
      const json& item = items[key.get<std::string>()];
      if (item["blob_id"] != blob_id || item["language"] != language ||
          item["size"] != size) {
        Invalid("inventory provenance differs from scoring item");
      }
      referenced.insert(key.get<std::string>());
    } else if (!key.is_null() || !reason.is_string()) {
      Invalid("unscorable inventory entry needs a reason and no item key");
    }
  }
  return referenced;
}

}  // namespace

json ResultRecord(const json& item, const std::string& fingerprint,
                  const FileTotals& totals) {
  return {{"key", item.at("key")},
          {"blob_id", item.at("blob_id")},
          {"language", item.at("language")},
          {"fingerprint", fingerprint},
          {"llm_cc", totals.llm_cc},
          {"token_count", totals.token_count},
          {"high_entropy_tokens", totals.high_entropy_tokens},
          {"entropy_sum", totals.entropy_sum},
          {"total_branch", totals.total_branch},
          {"total_comp_level", totals.total_comp_level}};
}

bool ValidResult(const json& result, const json& item,
                 const std::string& fingerprint) {
  if (!result.is_object() || result.size() != kResultFields.size() ||
      !item.is_object()) {
    return false;
  }
  for (const std::string_view field : kResultFields) {
    if (!result.contains(field)) {
      return false;
    }
  }
  return result["key"] == Field(item, "key") &&
         result["blob_id"] == Field(item, "blob_id") &&
         result["language"] == Field(item, "language") &&
         result["fingerprint"] == fingerprint &&
         IsFiniteNonNegative(result["llm_cc"]) &&
         IsCount(result["token_count"]) &&
         IsCount(result["high_entropy_tokens"]) &&
         result["high_entropy_tokens"].get<std::uint64_t>() <=
             result["token_count"].get<std::uint64_t>() &&
         IsFiniteNonNegative(result["entropy_sum"]) &&
         IsCount(result["total_branch"]) && IsCount(result["total_comp_level"]);
}

std::vector<WorkerAssignment> Partition(std::vector<json> items,
                                        int max_workers) {
  if (max_workers < 1 || max_workers > kMaxWorkers) {
    throw std::invalid_argument("max_workers must be between 1 and 4");
  }
  std::ranges::sort(items, [](const json& left, const json& right) {
    return std::tuple(-static_cast<double>(left["size"].get<std::uint64_t>()),
                      left["key"].get_ref<const std::string&>()) <
           std::tuple(-static_cast<double>(right["size"].get<std::uint64_t>()),
                      right["key"].get_ref<const std::string&>());
  });
  std::vector<WorkerAssignment> workers(std::min<std::size_t>(
      static_cast<std::size_t>(max_workers), items.size()));
  for (std::size_t index = 0; index < workers.size(); ++index) {
    workers[index].worker_id = static_cast<int>(index);
  }
  for (const json& item : items) {
    auto target = std::ranges::min_element(
        workers, {}, [](const WorkerAssignment& worker) {
          return std::pair(worker.bytes, worker.worker_id);
        });
    target->keys.push_back(item["key"].get<std::string>());
    target->bytes += item["size"].get<std::uint64_t>();
  }
  return workers;
}

json AssignmentsJson(const std::vector<WorkerAssignment>& workers) {
  json result = json::array();
  for (const WorkerAssignment& worker : workers) {
    result.push_back({{"worker_id", worker.worker_id},
                      {"keys", worker.keys},
                      {"bytes", worker.bytes}});
  }
  return result;
}

void ValidatePlan(const json& plan) {
  if (!plan.is_object() || Field(plan, "schema_version") != kSchemaVersion) {
    Invalid("unsupported plan schema version");
  }
  ValidateIdentity(Field(plan, "identity"));
  static_cast<void>(ModelFromJson(Field(plan, "model")));
  if (!Field(plan, "scorer").is_object()) {
    Invalid("plan needs a scorer identity");
  }
  // A fully cached plan reaches no worker, so its settings are checked here.
  static_cast<void>(SettingsFromScoring(Field(plan, "scoring")));
  const std::string fingerprint =
      Fingerprint(plan["scorer"], plan["model"], plan["scoring"]);
  if (Field(plan, "fingerprint") != fingerprint) {
    Invalid("plan fingerprint does not match its scorer, model and scoring");
  }
  if (!Field(plan, "max_file_bytes").is_number_unsigned() ||
      plan["max_file_bytes"].get<std::uint64_t>() == 0 ||
      plan["max_file_bytes"].get<std::uint64_t>() > kMaxSourceBytes) {
    Invalid("plan max_file_bytes must be between 1 and " +
            std::to_string(kMaxSourceBytes));
  }
  const json& items = Field(plan, "items");
  if (!items.is_object()) {
    Invalid("plan items must be an object");
  }
  std::set<std::string> item_keys;
  for (const auto& [key, item] : items.items()) {
    ValidateItem(key, item, fingerprint);
    item_keys.insert(key);
  }
  const json& inventories = Field(plan, "inventories");
  std::set<std::string> referenced;
  for (const char* side : {"base", "head"}) {
    for (const std::string& key :
         ValidateInventory(Field(inventories, side), items)) {
      referenced.insert(key);
    }
  }
  if (referenced != item_keys) {
    Invalid("plan items do not exactly match the inventories");
  }
  const json& hits = Field(plan, "hits");
  if (!hits.is_object()) {
    Invalid("plan hits must be an object");
  }
  std::set<std::string> misses = item_keys;
  for (const auto& [key, result] : hits.items()) {
    if (!items.contains(key) || !ValidResult(result, items[key], fingerprint)) {
      Invalid("plan contains an invalid cached result");
    }
    misses.erase(key);
  }
  const json& workers = Field(plan, "workers");
  if (!workers.is_array() || workers.size() > kMaxWorkers) {
    Invalid("plan workers must be a list of at most 4 assignments");
  }
  std::set<std::int64_t> worker_ids;
  std::set<std::string> assigned;
  std::size_t assignments = 0;
  for (const json& worker : workers) {
    const json& id = Field(worker, "worker_id");
    const json& keys = Field(worker, "keys");
    if (!IsInteger(id) || id.get<std::int64_t>() < 0 ||
        id.get<std::int64_t>() >= kMaxWorkers || !keys.is_array() ||
        keys.empty() || !Field(worker, "bytes").is_number_unsigned()) {
      Invalid("invalid worker assignment");
    }
    worker_ids.insert(id.get<std::int64_t>());
    std::uint64_t bytes = 0;
    for (const json& key : keys) {
      if (!key.is_string() || !items.contains(key.get<std::string>())) {
        Invalid("worker assignment names an unknown item");
      }
      ++assignments;
      assigned.insert(key.get<std::string>());
      bytes += items[key.get<std::string>()]["size"].get<std::uint64_t>();
    }
    if (bytes != worker["bytes"].get<std::uint64_t>()) {
      Invalid("worker assignment bytes do not match its items");
    }
  }
  if (worker_ids.size() != workers.size() || assigned.size() != assignments) {
    Invalid("plan has duplicate workers or assignments");
  }
  if (assigned != misses) {
    Invalid("worker assignments do not exactly cover cache misses");
  }
  if (!Field(plan, "changes").is_array() ||
      !Field(plan, "cache_stats").is_object()) {
    Invalid("plan needs changes and cache_stats");
  }
}

}  // namespace llmcc::compare
