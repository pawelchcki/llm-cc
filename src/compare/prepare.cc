#include "src/compare/prepare.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/compare/identity.h"
#include "src/compare/inventory.h"
#include "src/compare/json_util.h"
#include "src/compare/parallel.h"
#include "src/compare/plan.h"
#include "src/git.h"

namespace llmcc::compare {
namespace {

using nlohmann::json;

constexpr std::array<std::string_view, 8> kSuppliedIdentity = {
    "repository", "pipeline_id",   "head_sha",  "target_sha",
    "base_sha",   "target_branch", "pr_number", "started_at"};

json FullIdentity(const json& supplied, const std::string& head,
                  const std::string& target, const std::string& base,
                  const ResultCache::Clock& now) {
  if (!supplied.is_object()) {
    throw std::invalid_argument("identity must be a JSON object");
  }
  for (const auto& [name, value] : supplied.items()) {
    static_cast<void>(value);
    if (std::ranges::find(kSuppliedIdentity, name) == kSuppliedIdentity.end()) {
      throw std::invalid_argument(
          "identity fields must be repository, pipeline_id, head_sha, "
          "target_sha, base_sha, target_branch, pr_number and started_at; "
          "unexpected " +
          name);
    }
  }
  json identity = supplied;
  for (const char* required : {"repository", "pipeline_id"}) {
    if (!identity.contains(required) || !identity[required].is_string() ||
        identity[required].get_ref<const std::string&>().empty()) {
      throw std::invalid_argument(std::string("identity needs a ") + required);
    }
  }
  // Discovery leaves the commits it cannot know null; any it names must be
  // the ones compared.
  for (const auto& [field, commit] :
       {std::pair{"head_sha", &head}, std::pair{"target_sha", &target},
        std::pair{"base_sha", &base}}) {
    if (identity.contains(field) && !identity[field].is_null() &&
        identity[field] != *commit) {
      throw std::invalid_argument(std::string("identity ") + field +
                                  " differs from the resolved commit " +
                                  *commit);
    }
    identity[field] = *commit;
  }
  if (!identity.contains("target_branch")) {
    identity["target_branch"] = nullptr;
  }
  if (!identity.contains("pr_number")) {
    identity["pr_number"] = nullptr;
  }
  if (!identity.contains("started_at")) {
    identity["started_at"] =
        FormatTimestamp(now ? now() : std::chrono::system_clock::now());
  }
  return identity;
}

json Changes(const git::Repository& repository, const std::string& base,
             const std::string& head) {
  json changes = json::array();
  for (const git::Change& change : repository.Diff(base, head)) {
    const auto path = [](const std::optional<std::string>& value) {
      return value.has_value() ? json(PrintablePath(*value)) : json();
    };
    changes.push_back({{"status", change.status},
                       {"old_path", path(change.old_path)},
                       {"new_path", path(change.new_path)}});
  }
  return changes;
}

}  // namespace

json Prepare(const PrepareOptions& options) {
  if (options.max_workers < 1 || options.max_workers > kMaxWorkers) {
    throw std::invalid_argument("--max-workers must be between 1 and 4");
  }
  if (options.cache_concurrency < 1 || options.cache_concurrency > 64) {
    throw std::invalid_argument("--cache-concurrency must be between 1 and 64");
  }
  if (options.max_file_bytes == 0) {
    throw std::invalid_argument("--max-file-bytes must be positive");
  }
  const std::string fingerprint =
      Fingerprint(options.scorer, options.model, options.scoring);
  const git::Repository repository(options.repository);
  const std::string head = repository.ResolveCommit(options.head);
  const std::string target = repository.ResolveCommit(options.target);
  const std::string base = repository.MergeBase(target, head);
  const json identity =
      FullIdentity(options.identity, head, target, base, options.now);

  const TargetRules rules =
      LoadTargetRules(repository, target, options.default_rules);
  const json inventories = {
      {"base", BuildInventory(repository, base, rules.rules,
                              options.max_file_bytes, fingerprint)},
      {"head", BuildInventory(repository, head, rules.rules,
                              options.max_file_bytes, fingerprint)}};
  json items = json::object();
  for (const char* side : {"base", "head"}) {
    for (const json& file : inventories[side]) {
      if (file["scorable"].get<bool>() &&
          !items.contains(file["key"].get<std::string>())) {
        items[file["key"].get<std::string>()] = {{"key", file["key"]},
                                                 {"blob_id", file["blob_id"]},
                                                 {"language", file["language"]},
                                                 {"size", file["size"]}};
      }
    }
  }
  std::vector<json> ordered;
  for (const auto& [key, item] : items.items()) {
    static_cast<void>(key);
    ordered.push_back(item);
  }

  // Lookups run concurrently but land in key order, so the plan is the same
  // at any concurrency; the first store failure fails the run.
  std::vector<std::optional<json>> cached(ordered.size());
  if (options.cache != nullptr) {
    ResultCache cache(*options.cache, options.refresh_days, options.expire_days,
                      options.now);
    cached = BoundedMap<std::optional<json>>(
        ordered.size(), static_cast<std::size_t>(options.cache_concurrency),
        [&](std::size_t index) {
          return cache.Get(ordered[index], fingerprint);
        });
  }
  json hits = json::object();
  std::vector<json> misses;
  std::uint64_t hit_bytes = 0;
  std::uint64_t miss_bytes = 0;
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    const json& item = ordered[index];
    const std::uint64_t size = item["size"].get<std::uint64_t>();
    if (const std::optional<json>& hit = cached[index]; hit.has_value()) {
      hits[item["key"].get<std::string>()] = *hit;
      hit_bytes += size;
    } else {
      misses.push_back(item);
      miss_bytes += size;
    }
  }

  // Workers verify each blob against its object id. Rewriting every miss
  // repairs a blob an earlier, interrupted preparation left behind.
  std::vector<git::BlobRequest> requests;
  std::map<std::string, bool> requested;
  for (const json& item : misses) {
    const std::string blob_id = item["blob_id"];
    if (requested.emplace(blob_id, true).second) {
      requests.push_back(
          {.object_id = blob_id, .size = item["size"].get<std::uint64_t>()});
    }
  }
  std::filesystem::create_directories(options.output);
  repository.ReadBlobs(
      requests, [&](const std::string& blob_id, const std::string& contents) {
        WriteFileAtomic(options.output / "blobs" / blob_id, contents);
      });

  const std::vector<WorkerAssignment> workers =
      misses.empty() ? std::vector<WorkerAssignment>{}
                     : Partition(misses, options.max_workers);
  json plan = {{"schema_version", kSchemaVersion},
               {"identity", identity},
               {"scorer", options.scorer},
               {"model", options.model},
               {"scoring", options.scoring},
               {"fingerprint", fingerprint},
               {"max_file_bytes", options.max_file_bytes},
               {"rules", rules.rules.ToJson()},
               {"rules_source", rules.source},
               {"presentation", options.presentation},
               {"inventories", inventories},
               {"changes", Changes(repository, base, head)},
               {"items", items},
               {"hits", hits},
               {"workers", AssignmentsJson(workers)},
               {"cache_stats",
                {{"items", items.size()},
                 {"hits", hits.size()},
                 {"misses", misses.size()},
                 {"hit_bytes", hit_bytes},
                 {"miss_bytes", miss_bytes}}}};
  ValidatePlan(plan);
  WriteJsonFile(options.output / "plan.json", plan);
  return plan;
}

}  // namespace llmcc::compare
