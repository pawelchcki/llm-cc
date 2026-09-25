#include "src/compare/plan_v1_reader.h"

#include <cmath>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>

#include "src/compare/json_util.h"

namespace llmcc::compare::v1 {
namespace {

constexpr std::string_view kContainerEnvironmentPolicy = "sanitized-v1";

// Container profiles must record the sanitized worker environment, so results
// scored under an inherited environment are never reused.
void ValidateExecutionPolicy(const nlohmann::json& profile) {
  const nlohmann::json& build = profile.at("build");
  const nlohmann::json image =
      build.is_object() && build.contains("execution_image")
          ? build["execution_image"]
          : nlohmann::json();
  const nlohmann::json policy =
      build.is_object() && build.contains("container_environment_policy")
          ? build["container_environment_policy"]
          : nlohmann::json();
  if (!image.is_null() && image != "none" &&
      policy != kContainerEnvironmentPolicy) {
    throw std::invalid_argument(
        "container profile requires container_environment_policy=" +
        std::string(kContainerEnvironmentPolicy) +
        "; regenerate the scoring profile to invalidate inherited-environment "
        "results");
  }
}

std::string ItemKey(const nlohmann::json& item,
                    const std::string& fingerprint) {
  return CanonicalDigest(nlohmann::json::array(
      {item.at("content_sha256"), item.at("language"), fingerprint}));
}

bool Contains(const nlohmann::json& object, const std::string& key) {
  return object.is_object() && object.contains(key);
}

}  // namespace

std::string Fingerprint(const nlohmann::json& profile) {
  return CanonicalDigest(
      {{"scoring", profile.at("scoring")}, {"build", profile.at("build")}});
}

bool ValidResult(const nlohmann::json& result, const nlohmann::json& item,
                 const std::string& fingerprint) {
  if (!result.is_object() || !item.is_object()) {
    return false;
  }
  for (const char* field :
       {"schema_version", "key", "fingerprint", "content_sha256", "language",
        "token_count", "llm_cc"}) {
    if (!result.contains(field)) {
      return false;
    }
  }
  const nlohmann::json& tokens = result["token_count"];
  const nlohmann::json& score = result["llm_cc"];
  return result["schema_version"] == 1 && item.contains("key") &&
         result["key"] == item["key"] && result["fingerprint"] == fingerprint &&
         item.contains("content_sha256") &&
         result["content_sha256"] == item["content_sha256"] &&
         item.contains("language") && result["language"] == item["language"] &&
         IsInteger(tokens) && tokens.get<double>() >= 0 && IsNumber(score) &&
         std::isfinite(score.get<double>()) && score.get<double>() >= 0;
}

void ValidatePlan(const nlohmann::json& plan) {
  if (!plan.is_object() || !plan.contains("schema_version") ||
      plan["schema_version"] != kSchemaVersion) {
    throw std::invalid_argument("unsupported plan schema version");
  }
  const nlohmann::json& profile = plan.at("profile");
  ValidateExecutionPolicy(profile);
  const std::string expected_fingerprint = Fingerprint(profile);
  if (!Contains(plan, "fingerprint") ||
      plan["fingerprint"] != expected_fingerprint) {
    throw std::invalid_argument("plan fingerprint does not match its profile");
  }
  const nlohmann::json& items = plan.at("items");
  for (const auto& [key, item] : items.items()) {
    if (key != ItemKey(item, expected_fingerprint) || !Contains(item, "key") ||
        item["key"] != key) {
      throw std::invalid_argument(
          "plan item key does not match its provenance");
    }
  }
  std::set<std::string> referenced;
  for (const char* side : {"base", "head"}) {
    std::set<std::string> paths;
    for (const nlohmann::json& file : plan.at("inventories").at(side)) {
      if (!paths.insert(file.at("path").get<std::string>()).second) {
        throw std::invalid_argument("inventory has duplicate paths");
      }
      const nlohmann::json& category = file.at("category");
      if (category != "runtime" && category != "tests" &&
          category != "tooling") {
        throw std::invalid_argument("inventory has an invalid category");
      }
      const nlohmann::json key =
          file.contains("key") ? file["key"] : nlohmann::json();
      const nlohmann::json reason =
          file.contains("reason") ? file["reason"] : nlohmann::json();
      const bool scorable = file.at("scorable").get<bool>();
      if (scorable &&
          (!key.is_string() || !items.contains(key.get<std::string>()) ||
           !reason.is_null())) {
        throw std::invalid_argument(
            "inventory references an invalid scorable item");
      }
      if (!scorable && !key.is_null()) {
        throw std::invalid_argument(
            "unscorable inventory entry has an item key");
      }
      if (scorable) {
        const nlohmann::json& item = items[key.get<std::string>()];
        for (const char* field : {"content_sha256", "language", "size"}) {
          if (file.at(field) != item.at(field)) {
            throw std::invalid_argument(
                "inventory provenance differs from scoring item");
          }
        }
        referenced.insert(key.get<std::string>());
      }
    }
  }
  std::set<std::string> item_keys;
  for (const auto& [key, item] : items.items()) {
    static_cast<void>(item);
    item_keys.insert(key);
  }
  if (referenced != item_keys) {
    throw std::invalid_argument(
        "plan items do not exactly match the inventories");
  }
  const nlohmann::json hits =
      plan.contains("hits") ? plan["hits"] : nlohmann::json::object();
  std::set<std::string> hit_keys;
  for (const auto& [key, result] : hits.items()) {
    if (!items.contains(key) ||
        !ValidResult(result, items[key], expected_fingerprint)) {
      throw std::invalid_argument("plan contains an invalid cached result");
    }
    hit_keys.insert(key);
  }
  std::set<std::int64_t> worker_ids;
  std::set<std::string> assigned;
  std::size_t assignments = 0;
  std::size_t workers = 0;
  for (const nlohmann::json& worker : plan.at("workers")) {
    const nlohmann::json& id = worker.at("worker_id");
    const nlohmann::json& keys = worker.at("keys");
    if (!IsInteger(id) || id.get<std::int64_t>() < 0 ||
        id.get<std::int64_t>() >= 4 || !keys.is_array() || keys.empty()) {
      throw std::invalid_argument("invalid worker assignment");
    }
    ++workers;
    worker_ids.insert(id.get<std::int64_t>());
    for (const nlohmann::json& key : keys) {
      ++assignments;
      assigned.insert(key.get<std::string>());
    }
  }
  if (worker_ids.size() != workers || assigned.size() != assignments) {
    throw std::invalid_argument("plan has duplicate workers or assignments");
  }
  std::set<std::string> misses;
  for (const std::string& key : item_keys) {
    if (!hit_keys.contains(key)) {
      misses.insert(key);
    }
  }
  if (assigned != misses) {
    throw std::invalid_argument(
        "worker assignments do not exactly cover cache misses");
  }
}

}  // namespace llmcc::compare::v1
