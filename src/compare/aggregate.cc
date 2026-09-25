#include "src/compare/aggregate.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "src/compare/identity.h"
#include "src/compare/json_util.h"
#include "src/compare/markdown.h"
#include "src/compare/plan.h"
#include "src/compare/report_model.h"
#include "src/compare/report_render.h"

namespace llmcc::compare {
namespace {

using nlohmann::json;

constexpr std::array<std::string_view, 8> kIdentityFields = {
    "repository", "pipeline_id",   "head_sha",  "target_sha",
    "base_sha",   "target_branch", "pr_number", "started_at"};

const json& Get(const json& object, std::string_view key) {
  static const json kNull;
  if (!object.is_object()) {
    return kNull;
  }
  const auto found = object.find(key);
  return found == object.end() ? kNull : *found;
}

std::set<std::string> ObjectKeys(const json& object) {
  std::set<std::string> keys;
  if (object.is_object()) {
    for (const auto& [key, value] : object.items()) {
      static_cast<void>(value);
      keys.insert(key);
    }
  }
  return keys;
}

bool WellFormedArtifact(const json& artifact) {
  if (!artifact.is_object() || !IsInteger(Get(artifact, "worker_id")) ||
      !Get(artifact, "results").is_object() ||
      !Get(artifact, "errors").is_array()) {
    return false;
  }
  return std::ranges::all_of(
      artifact["errors"], [](const json& error) { return error.is_string(); });
}

// Merges every valid worker's results into `results`, reporting each missing,
// foreign, duplicate or incomplete worker in `errors`.
void CollectWorkers(const json& plan, const std::vector<std::string>& paths,
                    json& results, std::vector<std::string>& errors) {
  std::map<std::int64_t, std::set<std::string>> expected_workers;
  for (const json& worker : plan["workers"]) {
    std::set<std::string>& keys =
        expected_workers[worker["worker_id"].get<std::int64_t>()];
    for (const json& key : worker["keys"]) {
      keys.insert(key.get<std::string>());
    }
  }
  const std::string fingerprint = plan["fingerprint"].get<std::string>();
  std::set<std::int64_t> seen;
  for (const std::string& path : paths) {
    json artifact;
    try {
      artifact = ReadJsonFile(std::filesystem::u8path(path));
    } catch (const std::exception& error) {
      errors.push_back("cannot read worker artifact " + path + ": " +
                       error.what());
      continue;
    }
    if (!WellFormedArtifact(artifact)) {
      errors.push_back("malformed worker artifact: " + path);
      continue;
    }
    const std::int64_t worker_id = artifact["worker_id"].get<std::int64_t>();
    const std::string worker = "worker " + std::to_string(worker_id);
    if (Get(artifact, "identity") != plan["identity"] ||
        Get(artifact, "fingerprint") != plan["fingerprint"]) {
      errors.push_back(worker + " belongs to another pipeline or fingerprint");
      continue;
    }
    if (!expected_workers.contains(worker_id) || seen.contains(worker_id)) {
      errors.push_back("unexpected or duplicate " + worker);
      continue;
    }
    seen.insert(worker_id);
    const std::set<std::string> actual = ObjectKeys(artifact["results"]);
    const std::set<std::string>& expected = expected_workers.at(worker_id);
    std::vector<std::string> artifact_errors;
    if (Get(artifact, "schema_version") != kSchemaVersion) {
      artifact_errors.emplace_back("unsupported schema");
    }
    if (actual != expected) {
      std::size_t missing = 0;
      std::size_t extra = 0;
      for (const std::string& key : expected) {
        missing += actual.contains(key) ? 0 : 1;
      }
      for (const std::string& key : actual) {
        extra += expected.contains(key) ? 0 : 1;
      }
      artifact_errors.push_back("result coverage mismatch (missing " +
                                std::to_string(missing) + ", extra " +
                                std::to_string(extra) + ")");
    }
    if (Get(artifact, "status") != "complete") {
      std::string joined;
      for (const json& error : artifact["errors"]) {
        joined += joined.empty() ? "" : "; ";
        joined += error.get<std::string>();
      }
      artifact_errors.push_back("failed: " + joined);
    } else if (!artifact["errors"].empty()) {
      artifact_errors.emplace_back("complete worker reported errors");
    }
    for (const auto& [key, result] : artifact["results"].items()) {
      const json& item = Get(plan["items"], key);
      if (expected.contains(key) && !item.is_null() && !item.empty() &&
          !ValidResult(result, item, fingerprint)) {
        artifact_errors.push_back("invalid result " + key);
      }
    }
    if (!artifact_errors.empty()) {
      for (const std::string& error : artifact_errors) {
        errors.push_back(worker);
        errors.back().append(" ").append(error);
      }
      continue;
    }
    for (const auto& [key, result] : artifact["results"].items()) {
      results[key] = result;
    }
  }
  std::string missing_workers;
  for (const auto& [worker_id, keys] : expected_workers) {
    static_cast<void>(keys);
    if (!seen.contains(worker_id)) {
      missing_workers +=
          (missing_workers.empty() ? "" : ", ") + std::to_string(worker_id);
    }
  }
  if (!missing_workers.empty()) {
    errors.push_back("missing worker artifacts: " + missing_workers);
  }
}

json Aggregate(const json& plan, const std::vector<std::string>& worker_paths,
               const std::filesystem::path& output) {
  ValidatePlan(plan);
  ReportInputs inputs{
      .schema_version = kSchemaVersion,
      .identity = plan["identity"],
      .fingerprint = plan["fingerprint"],
      .header = {{"scorer", plan["scorer"]},
                 {"model", plan["model"]},
                 {"scoring", plan["scoring"]}},
      .inventories = plan["inventories"],
      .changes = plan["changes"],
      .results = plan.contains("hits") ? plan["hits"] : json::object(),
      .rules_source = Get(plan, "rules_source"),
      .presentation = Get(plan, "presentation"),
      .cache_stats = plan["cache_stats"]};
  CollectWorkers(plan, worker_paths, inputs.results, inputs.errors);
  for (const auto& [key, item] : plan["items"].items()) {
    static_cast<void>(item);
    inputs.missing += inputs.results.contains(key) ? 0 : 1;
  }
  if (inputs.missing != 0) {
    inputs.errors.push_back("unmeasured unique files: " +
                            std::to_string(inputs.missing));
  }
  const json report = BuildReport(inputs);
  WriteReportArtifacts(output, report, RenderFullReport(report),
                       RenderComment(report));
  return report;
}

}  // namespace

nlohmann::json AggregatePlan(const std::filesystem::path& plan_path,
                             const std::vector<std::string>& worker_paths,
                             const std::filesystem::path& output) {
  json plan;
  try {
    plan = ReadJsonFile(plan_path);
    return Aggregate(plan, worker_paths, output);
  } catch (const std::exception& error) {
    return WriteFailureReport(
        output, Get(plan, "identity"), Get(plan, "fingerprint"),
        {std::string("aggregation failed: ") + error.what()});
  }
}

nlohmann::json WriteFailureReport(const std::filesystem::path& output,
                                  const nlohmann::json& identity,
                                  const nlohmann::json& fingerprint,
                                  const std::vector<std::string>& errors) {
  json normalized = json::object();
  for (const std::string_view field : kIdentityFields) {
    normalized[std::string(field)] = Get(identity, field);
  }
  const json unavailable = {{"score", Delta(json(), json())},
                            {"raw_llm_cc", Delta(json(), json())},
                            {"tokens", Delta(json(), json())},
                            {"coverage", {{"base", 0.0}, {"head", 0.0}}}};
  json errors_json = json::array();
  std::string markdown =
      "## llm-cc comparison\n\nStatus: **failed**\n\nErrors:\n";
  for (const std::string& error : errors) {
    errors_json.push_back(error);
    markdown += "- " + markdown::Code(error) + "\n";
  }
  const json empty = json::array();
  json report = {{"schema_version", kSchemaVersion},
                 {"identity", normalized},
                 {"fingerprint", fingerprint},
                 {"status", "failed"},
                 {"scorer", nullptr},
                 {"model", nullptr},
                 {"scoring", nullptr},
                 {"inventories", {{"base", empty}, {"head", empty}}},
                 {"results", json::object()},
                 {"sides",
                  {{"base", Side(empty, json::object())},
                   {"head", Side(empty, json::object())}}},
                 {"comparisons",
                  {{"runtime", unavailable},
                   {"tests", unavailable},
                   {"tooling", unavailable},
                   {"repository", unavailable}}},
                 {"changes", empty},
                 {"change_counts",
                  {{"additions", 0},
                   {"deletions", 0},
                   {"renames", 0},
                   {"category_moves", 0}}},
                 {"rankings", {{"base", empty}, {"head", empty}}},
                 {"changed_files", empty},
                 {"leading_regressions", empty},
                 {"leading_improvements", empty},
                 {"rules_source", {{"source", "host"}}},
                 {"presentation", json::object()},
                 {"cache_stats",
                  {{"items", 0},
                   {"hits", 0},
                   {"misses", 0},
                   {"hit_bytes", 0},
                   {"miss_bytes", 0}}},
                 {"errors", std::move(errors_json)}};
  std::string trimmed = markdown;
  while (!trimmed.empty() && trimmed.back() == '\n') {
    trimmed.pop_back();
  }
  markdown::Section section{.priority = 1};
  std::size_t start = 0;
  while (true) {
    const std::size_t end = trimmed.find('\n', start);
    section.lines.push_back(trimmed.substr(start, end - start));
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  WriteReportArtifacts(output, report, markdown, markdown::Assemble({section}));
  return report;
}

}  // namespace llmcc::compare
