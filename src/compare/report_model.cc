#include "src/compare/report_model.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "src/compare/json_util.h"

namespace llmcc::compare {
namespace {

using nlohmann::json;

constexpr std::array<std::string_view, 3> kCategories = {"runtime", "tests",
                                                         "tooling"};

const json& Get(const json& object, std::string_view key) {
  static const json kNull;
  if (!object.is_object()) {
    return kNull;
  }
  const auto found = object.find(key);
  return found == object.end() ? kNull : *found;
}

// Python arithmetic on JSON numbers: integers stay integers.
json Subtract(const json& head, const json& base) {
  if (IsInteger(head) && IsInteger(base)) {
    return head.get<std::int64_t>() - base.get<std::int64_t>();
  }
  return head.get<double>() - base.get<double>();
}

json DeltaOf(const json& base, const json& head) {
  const json absolute =
      base.is_null() || head.is_null() ? json() : Subtract(head, base);
  const json percent =
      absolute.is_null() || base.get<double>() == 0.0
          ? json()
          : json(absolute.get<double>() / base.get<double>() * 100);
  return {{"base", base},
          {"head", head},
          {"absolute", absolute},
          {"percent", percent}};
}

json Ratio(std::int64_t numerator, std::int64_t denominator) {
  return denominator == 0 ? json(1.0)
                          : json(static_cast<double>(numerator) /
                                 static_cast<double>(denominator));
}

// Per-token score; null without tokens.
json PerToken(const json& result) {
  const json& tokens = result["token_count"];
  return tokens.get<double>() == 0.0
             ? json()
             : json(result["llm_cc"].get<double>() / tokens.get<double>());
}

const json* ResultFor(const json& results, const json& file) {
  const json& key = Get(file, "key");
  if (!key.is_string() || key.get_ref<const std::string&>().empty()) {
    return nullptr;
  }
  const auto found = results.find(key.get<std::string>());
  return found == results.end() ? nullptr : &*found;
}

json SideOf(const json& inventory, const json& results) {
  struct Category {
    std::int64_t paths = 0;
    std::int64_t supported_paths = 0;
    std::int64_t measured_paths = 0;
    std::int64_t bytes = 0;
    std::int64_t supported_bytes = 0;
    std::int64_t measured_bytes = 0;
    double measured_llm_cc = 0.0;
    std::int64_t measured_token_count = 0;
  };
  std::map<std::string, Category, std::less<>> categories;
  for (const std::string_view name : kCategories) {
    categories[std::string(name)];
  }
  std::map<std::string, std::int64_t> reasons;
  std::int64_t supported = 0;
  std::int64_t measured = 0;
  std::int64_t supported_bytes = 0;
  std::int64_t measured_bytes = 0;
  for (const json& file : inventory) {
    Category& category = categories.at(file.at("category").get<std::string>());
    const json& size = file.at("size");
    const std::int64_t bytes = size.is_null() ? 0 : size.get<std::int64_t>();
    ++category.paths;
    category.bytes += bytes;
    const bool scorable = file.at("scorable").get<bool>();
    const bool supported_file =
        scorable || (!Get(file, "language").is_null() &&
                     Get(file, "reason") == "oversized");
    if (supported_file) {
      ++supported;
      ++category.supported_paths;
      supported_bytes += bytes;
      category.supported_bytes += bytes;
    }
    if (scorable) {
      if (const json* result = ResultFor(results, file); result != nullptr) {
        ++measured;
        measured_bytes += bytes;
        ++category.measured_paths;
        category.measured_bytes += bytes;
        category.measured_llm_cc += (*result)["llm_cc"].get<double>();
        category.measured_token_count +=
            (*result)["token_count"].get<std::int64_t>();
      }
    } else {
      ++reasons[file.at("reason").get<std::string>()];
    }
  }
  json category_json = json::object();
  double measured_llm_cc = 0.0;
  std::int64_t measured_tokens = 0;
  for (const std::string_view name : kCategories) {
    const Category& category = categories.at(std::string(name));
    const bool complete = category.measured_paths == category.supported_paths;
    const json llm_cc = complete ? json(category.measured_llm_cc) : json();
    const json tokens = complete ? json(category.measured_token_count) : json();
    category_json[std::string(name)] = {
        {"paths", category.paths},
        {"supported_paths", category.supported_paths},
        {"measured_paths", category.measured_paths},
        {"bytes", category.bytes},
        {"supported_bytes", category.supported_bytes},
        {"measured_bytes", category.measured_bytes},
        {"measured_llm_cc", category.measured_llm_cc},
        {"measured_token_count", category.measured_token_count},
        {"llm_cc", llm_cc},
        {"token_count", tokens},
        {"score", complete && category.measured_token_count != 0
                      ? json(category.measured_llm_cc /
                             static_cast<double>(category.measured_token_count))
                      : json()},
        {"coverage", Ratio(category.measured_paths, category.supported_paths)},
        {"byte_coverage",
         Ratio(category.measured_bytes, category.supported_bytes)}};
    measured_llm_cc += category.measured_llm_cc;
    measured_tokens += category.measured_token_count;
  }
  const bool complete = measured == supported;
  json reason_json = json::object();
  for (const auto& [reason, count] : reasons) {
    reason_json[reason] = count;
  }
  json totals = {{"paths", inventory.size()},
                 {"supported_paths", supported},
                 {"measured_paths", measured},
                 {"coverage", Ratio(measured, supported)},
                 {"unmeasured_reasons", std::move(reason_json)},
                 {"supported_bytes", supported_bytes},
                 {"measured_bytes", measured_bytes},
                 {"byte_coverage", Ratio(measured_bytes, supported_bytes)},
                 {"measured_llm_cc", measured_llm_cc},
                 {"measured_token_count", measured_tokens},
                 {"llm_cc", complete ? json(measured_llm_cc) : json()},
                 {"token_count", complete ? json(measured_tokens) : json()},
                 {"score", complete && measured_tokens != 0
                               ? json(measured_llm_cc /
                                      static_cast<double>(measured_tokens))
                               : json()}};
  return {{"totals", std::move(totals)},
          {"categories", std::move(category_json)}};
}

// Measured files ordered by total LM-CC, with per-token data alongside.
json RankingsOf(const json& inventory, const json& results,
                const std::set<std::string>& changed_paths) {
  std::vector<json> entries;
  for (const json& file : inventory) {
    if (!file.at("scorable").get<bool>()) {
      continue;
    }
    const json* result = ResultFor(results, file);
    if (result == nullptr) {
      continue;
    }
    const std::string path = file.at("path").get<std::string>();
    entries.push_back({{"path", path},
                       {"category", file.at("category")},
                       {"language", file.at("language")},
                       {"size", file.at("size")},
                       {"score", PerToken(*result)},
                       {"llm_cc", (*result)["llm_cc"]},
                       {"token_count", (*result)["token_count"]},
                       {"rank", 0},
                       {"category_rank", 0},
                       {"changed", changed_paths.contains(path)}});
  }
  std::ranges::stable_sort(entries, [](const json& left, const json& right) {
    return std::tuple(-left["llm_cc"].get<double>(),
                      left["path"].get_ref<const std::string&>()) <
           std::tuple(-right["llm_cc"].get<double>(),
                      right["path"].get_ref<const std::string&>());
  });
  std::map<std::string, std::int64_t> seen;
  std::int64_t position = 0;
  json ranked = json::array();
  for (json& entry : entries) {
    entry["rank"] = ++position;
    entry["category_rank"] = ++seen[entry["category"].get<std::string>()];
    ranked.push_back(std::move(entry));
  }
  return ranked;
}

using PathIndex = std::map<std::string, const json*, std::less<>>;

PathIndex IndexPaths(const json& inventory) {
  PathIndex index;
  for (const json& file : inventory) {
    index[file.at("path").get<std::string>()] = &file;
  }
  return index;
}

const json* Lookup(const PathIndex& index, const json& path) {
  if (!path.is_string()) {
    return nullptr;
  }
  const auto found = index.find(path.get_ref<const std::string&>());
  return found == index.end() ? nullptr : found->second;
}

// One row per change that carries a score on either side.
json ChangedFilesOf(const json& changes, const PathIndex& base_paths,
                    const PathIndex& head_paths, const json& results,
                    const json& head_rankings) {
  std::map<std::string, const json*, std::less<>> ranked;
  for (const json& entry : head_rankings) {
    ranked[entry["path"].get<std::string>()] = &entry;
  }
  const auto measurement = [&](const json* file) {
    const json* result = file == nullptr ? nullptr : ResultFor(results, *file);
    if (result == nullptr) {
      return std::pair(json(), json());
    }
    return std::pair((*result)["llm_cc"], PerToken(*result));
  };
  std::vector<json> rows;
  for (const json& change : changes) {
    const json* old_file = Lookup(base_paths, change["old_path"]);
    const json* new_file = Lookup(head_paths, change["new_path"]);
    const auto [base_raw, base_score] = measurement(old_file);
    const auto [head_raw, head_score] = measurement(new_file);
    if (base_raw.is_null() && head_raw.is_null()) {
      continue;
    }
    const json& path =
        change["new_path"].is_null() ? change["old_path"] : change["new_path"];
    const json delta = DeltaOf(base_score, head_score);
    const json raw_delta = DeltaOf(base_raw, head_raw);
    const json* rank = nullptr;
    if (new_file != nullptr) {
      const auto found = ranked.find((*new_file)["path"].get<std::string>());
      rank = found == ranked.end() ? nullptr : found->second;
    }
    json base = json();
    if (old_file != nullptr) {
      base = {{"category", (*old_file)["category"]},
              {"score", base_score},
              {"llm_cc", base_raw}};
    }
    json head = json();
    if (new_file != nullptr) {
      head = {{"category", (*new_file)["category"]},
              {"score", head_score},
              {"llm_cc", head_raw},
              {"rank", rank != nullptr ? (*rank)["rank"] : json()},
              {"category_rank",
               rank != nullptr ? (*rank)["category_rank"] : json()}};
    }
    rows.push_back({{"status", change["status"]},
                    {"old_path", change["old_path"]},
                    {"new_path", change["new_path"]},
                    {"path", path},
                    {"base", std::move(base)},
                    {"head", std::move(head)},
                    {"delta", delta["absolute"]},
                    {"percent", delta["percent"]},
                    {"raw_delta", raw_delta["absolute"]},
                    {"raw_percent", raw_delta["percent"]}});
  }
  std::ranges::stable_sort(rows, [](const json& left, const json& right) {
    return left["path"].get_ref<const std::string&>() <
           right["path"].get_ref<const std::string&>();
  });
  return json(rows);
}

json Comparison(const json& base, const json& head) {
  return {{"score", DeltaOf(base["score"], head["score"])},
          {"raw_llm_cc", DeltaOf(base["llm_cc"], head["llm_cc"])},
          {"tokens", DeltaOf(base["token_count"], head["token_count"])}};
}

}  // namespace

nlohmann::json Delta(const nlohmann::json& base, const nlohmann::json& head) {
  return DeltaOf(base, head);
}

nlohmann::json Side(const nlohmann::json& inventory,
                    const nlohmann::json& results) {
  return SideOf(inventory, results);
}

nlohmann::json Rankings(const nlohmann::json& inventory,
                        const nlohmann::json& results,
                        const std::set<std::string>& changed_paths) {
  return RankingsOf(inventory, results, changed_paths);
}

nlohmann::json ChangedFiles(const nlohmann::json& changes,
                            const nlohmann::json& base_inventory,
                            const nlohmann::json& head_inventory,
                            const nlohmann::json& results,
                            const nlohmann::json& head_rankings) {
  return ChangedFilesOf(changes, IndexPaths(base_inventory),
                        IndexPaths(head_inventory), results, head_rankings);
}

nlohmann::json BuildReport(const ReportInputs& inputs) {
  const json& results = inputs.results;
  const json& inventories = inputs.inventories;
  const json base = SideOf(inventories["base"], results);
  const json head = SideOf(inventories["head"], results);
  json comparisons = json::object();
  for (const std::string_view name : kCategories) {
    const json& base_category = base["categories"][std::string(name)];
    const json& head_category = head["categories"][std::string(name)];
    json comparison = Comparison(base_category, head_category);
    comparison["coverage"] = {
        {"base", Ratio(base_category["measured_paths"].get<std::int64_t>(),
                       base_category["supported_paths"].get<std::int64_t>())},
        {"head", Ratio(head_category["measured_paths"].get<std::int64_t>(),
                       head_category["supported_paths"].get<std::int64_t>())}};
    comparisons[std::string(name)] = std::move(comparison);
  }
  json repository = Comparison(base["totals"], head["totals"]);
  repository["coverage"] = {{"base", base["totals"]["coverage"]},
                            {"head", head["totals"]["coverage"]}};
  comparisons["repository"] = std::move(repository);

  const json& changes = inputs.changes;
  const PathIndex base_paths = IndexPaths(inventories["base"]);
  const PathIndex head_paths = IndexPaths(inventories["head"]);
  json counts = {{"additions", 0},
                 {"deletions", 0},
                 {"renames", 0},
                 {"category_moves", 0}};
  std::set<std::string> base_changed;
  std::set<std::string> head_changed;
  for (const json& change : changes) {
    const std::string status = change["status"].get<std::string>();
    if (status.starts_with("A")) {
      counts["additions"] = counts["additions"].get<int>() + 1;
    } else if (status.starts_with("D")) {
      counts["deletions"] = counts["deletions"].get<int>() + 1;
    } else if (status.starts_with("R")) {
      counts["renames"] = counts["renames"].get<int>() + 1;
    }
    const json* old_file = Lookup(base_paths, change["old_path"]);
    const json* new_file = Lookup(head_paths, change["new_path"]);
    if (old_file != nullptr && new_file != nullptr &&
        (*old_file)["category"] != (*new_file)["category"]) {
      counts["category_moves"] = counts["category_moves"].get<int>() + 1;
    }
    if (Truthy(change["old_path"])) {
      base_changed.insert(change["old_path"].get<std::string>());
    }
    if (Truthy(change["new_path"])) {
      head_changed.insert(change["new_path"].get<std::string>());
    }
  }
  const json rankings = {
      {"base", RankingsOf(inventories["base"], results, base_changed)},
      {"head", RankingsOf(inventories["head"], results, head_changed)}};
  const json changed_files = ChangedFilesOf(changes, base_paths, head_paths,
                                            results, rankings["head"]);

  // Raw LM-CC is the paper's quantity and the displayed headline. It stays
  // defined for zero-token files, so candidates come from every change;
  // `change` keeps the per-token delta (null without tokens).
  const auto raw_llm_cc = [&](const PathIndex& paths, const json& path) {
    // A side that does not exist contributes nothing; a side that exists
    // but was not measured leaves the raw delta undefined.
    if (path.is_null()) {
      return json(0.0);
    }
    const json* file = Lookup(paths, path);
    const json* result = file == nullptr ? nullptr : ResultFor(results, *file);
    return result == nullptr ? json() : (*result)["llm_cc"];
  };
  std::map<std::pair<std::string, std::string>, json> per_token;
  const auto change_key = [](const json& old_path, const json& new_path) {
    return std::pair(
        old_path.is_null() ? std::string("\x01") : old_path.dump(),
        new_path.is_null() ? std::string("\x01") : new_path.dump());
  };
  for (const json& row : changed_files) {
    per_token[change_key(row["old_path"], row["new_path"])] = row["delta"];
  }
  std::vector<json> path_deltas;
  for (const json& change : changes) {
    const json base_raw = raw_llm_cc(base_paths, change["old_path"]);
    const json head_raw = raw_llm_cc(head_paths, change["new_path"]);
    if (base_raw.is_null() || head_raw.is_null()) {
      continue;
    }
    const auto found =
        per_token.find(change_key(change["old_path"], change["new_path"]));
    path_deltas.push_back(
        {{"path", change["new_path"].is_null() ? change["old_path"]
                                               : change["new_path"]},
         {"change", found == per_token.end() ? json() : found->second},
         {"raw_change", Subtract(head_raw, base_raw)}});
  }
  const auto leading = [&](bool regressions) {
    std::vector<json> selected;
    for (const json& delta : path_deltas) {
      const double change = delta["raw_change"].get<double>();
      if (regressions ? change > 0 : change < 0) {
        selected.push_back(delta);
      }
    }
    std::ranges::stable_sort(
        selected, [&](const json& left, const json& right) {
          const double sign = regressions ? -1.0 : 1.0;
          return std::tuple(sign * left["raw_change"].get<double>(),
                            left["path"].get_ref<const std::string&>()) <
                 std::tuple(sign * right["raw_change"].get<double>(),
                            right["path"].get_ref<const std::string&>());
        });
    if (selected.size() > 10) {
      selected.resize(10);
    }
    return json(selected);
  };

  const bool incomplete_coverage =
      base["totals"]["coverage"].get<double>() < 1.0 ||
      head["totals"]["coverage"].get<double>() < 1.0;
  const std::string status =
      !inputs.errors.empty()
          ? "failed"
          : (inputs.missing != 0 || incomplete_coverage ? "incomplete"
                                                        : "complete");
  json report_changes = json::array();
  for (const json& change : changes) {
    json enriched = change;
    const json* old_file = Lookup(base_paths, change["old_path"]);
    const json* new_file = Lookup(head_paths, change["new_path"]);
    const auto side = [&](const json* file) {
      if (file == nullptr) {
        return json();
      }
      const json* result = ResultFor(results, *file);
      return json{{"category", (*file)["category"]},
                  {"score", result == nullptr ? json() : PerToken(*result)}};
    };
    enriched["base"] = side(old_file);
    enriched["head"] = side(new_file);
    report_changes.push_back(std::move(enriched));
  }
  json errors_json = json::array();
  for (const std::string& error : inputs.errors) {
    errors_json.push_back(error);
  }
  const json& rules_source = inputs.rules_source;
  const json& presentation = inputs.presentation;
  json report = {
      {"schema_version", inputs.schema_version},
      {"identity", inputs.identity},
      {"fingerprint", inputs.fingerprint},
      {"status", status},
      {"inventories", inventories},
      {"results", results},
      {"sides", {{"base", base}, {"head", head}}},
      {"comparisons", std::move(comparisons)},
      {"changes", std::move(report_changes)},
      {"change_counts", std::move(counts)},
      {"rankings", rankings},
      {"changed_files", changed_files},
      {"leading_regressions", leading(true)},
      {"leading_improvements", leading(false)},
      {"rules_source",
       Truthy(rules_source) ? rules_source : json{{"source", "host"}}},
      {"presentation", Truthy(presentation) ? presentation : json::object()},
      {"cache_stats", inputs.cache_stats},
      {"errors", std::move(errors_json)}};
  for (const auto& [key, value] : inputs.header.items()) {
    report[key] = value;
  }
  return report;
}

}  // namespace llmcc::compare
