#include "src/compare/report_render.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "src/compare/json_util.h"
#include "src/compare/markdown.h"
#include "src/sha256.h"

namespace llmcc::compare {
namespace {

using markdown::Code;
using markdown::Fmt;
using markdown::Percent;
using markdown::Raw;
using markdown::Section;

constexpr std::size_t kChangedFilesInline = 25;
constexpr std::array<std::string_view, 3> kCategories = {"runtime", "tests",
                                                         "tooling"};
constexpr std::array<std::string_view, 4> kComparisons = {
    "runtime", "tests", "tooling", "repository"};
constexpr std::string_view kDash = "\xE2\x80\x94";
constexpr std::string_view kEmptySet = "\xE2\x88\x85";
constexpr std::string_view kArrow = "\xE2\x86\x92";
constexpr std::string_view kDelta = "\xCE\x94";

// `object.get(key)`, null for a missing key or a non-object.
const nlohmann::json& Get(const nlohmann::json& object, std::string_view key) {
  static const nlohmann::json kNull;
  if (!object.is_object()) {
    return kNull;
  }
  const auto found = object.find(key);
  return found == object.end() ? kNull : *found;
}

// `object.get(key) or fallback` for containers.
const nlohmann::json& GetOr(const nlohmann::json& object, std::string_view key,
                            const nlohmann::json& fallback) {
  const nlohmann::json& value = Get(object, key);
  return Truthy(value) ? value : fallback;
}

const nlohmann::json& EmptyObject() {
  static const nlohmann::json kEmpty = nlohmann::json::object();
  return kEmpty;
}

const nlohmann::json& EmptyArray() {
  static const nlohmann::json kEmpty = nlohmann::json::array();
  return kEmpty;
}

// Python's str() of a scalar as "%s" formats it.
std::string Str(const nlohmann::json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_null()) {
    return "None";
  }
  if (value.is_boolean()) {
    return value.get<bool>() ? "True" : "False";
  }
  if (value.is_number_integer()) {
    return value.dump();
  }
  return CanonicalJson(value);
}

std::string Integer(const nlohmann::json& value) {
  return std::to_string(static_cast<long long>(value.get<double>()));
}

std::string Percentage(double value) {
  std::array<char, 64> buffer{};
  const int size =
      std::snprintf(buffer.data(), buffer.size(), "%.1f%%", value);  // NOLINT
  return {buffer.data(), static_cast<std::size_t>(std::max(size, 0))};
}

std::string Join(const std::vector<std::string>& lines) {
  std::string text;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    if (index != 0) {
      text.push_back('\n');
    }
    text += lines[index];
  }
  return text;
}

std::string Title(std::string_view side) {
  std::string title(side);
  title.front() = static_cast<char>(title.front() - 'a' + 'A');
  return title;
}

std::string PathChange(const nlohmann::json& delta) {
  std::array<char, 128> buffer{};
  int size = 0;
  if (!delta.contains("raw_change")) {  // Reports written before raw headlines.
    size = std::snprintf(buffer.data(), buffer.size(),  // NOLINT
                         "%+.6g LM-CC/token", delta["change"].get<double>());
  } else if (delta["change"].is_null()) {  // Zero tokens on one side.
    size = std::snprintf(buffer.data(), buffer.size(),  // NOLINT
                         "%+.1f LM-CC", delta["raw_change"].get<double>());
  } else {
    size = std::snprintf(buffer.data(), buffer.size(),  // NOLINT
                         "%+.1f LM-CC (%+.3g LM-CC/token)",
                         delta["raw_change"].get<double>(),
                         delta["change"].get<double>());
  }
  return {buffer.data(), static_cast<std::size_t>(std::max(size, 0))};
}

std::vector<std::string> Headline(const nlohmann::json& report) {
  std::vector<std::string> lines;
  const nlohmann::json& comparisons =
      GetOr(report, "comparisons", EmptyObject());
  for (const std::string_view name : kComparisons) {
    const nlohmann::json& item = Get(comparisons, name);
    if (!Truthy(item)) {
      continue;
    }
    const nlohmann::json& raw = item["raw_llm_cc"];
    lines.push_back("- " + std::string(name) + " " + Raw(raw["base"]) + " " +
                    std::string(kArrow) + " " + Raw(raw["head"]) + " LM-CC (" +
                    Percent(raw["percent"]) + ")");
  }
  return lines;
}

std::vector<std::string> CategoryTable(const nlohmann::json& report) {
  const std::string delta(kDelta);
  std::vector<std::string> lines = {
      "",
      "| Category | LM-CC base | LM-CC head | LM-CC " + delta + " | LM-CC " +
          delta + "% | LM-CC/token " + delta + " | Tokens " + delta +
          " | Coverage |",
      "|---|---:|---:|---:|---:|---:|---:|---:|",
  };
  const nlohmann::json& comparisons =
      GetOr(report, "comparisons", EmptyObject());
  for (const std::string_view name : kComparisons) {
    const nlohmann::json& item = Get(comparisons, name);
    if (!Truthy(item)) {
      continue;
    }
    const nlohmann::json& raw = item["raw_llm_cc"];
    lines.push_back(
        "| " + std::string(name) + " | " + Raw(raw["base"]) + " | " +
        Raw(raw["head"]) + " | " + Raw(raw["absolute"], true) + " | " +
        Percent(raw["percent"]) + " | " + Fmt(item["score"]["absolute"]) +
        " | " + Fmt(item["tokens"]["absolute"]) + " | " +
        Percentage(item["coverage"]["base"].get<double>() * 100) + " / " +
        Percentage(item["coverage"]["head"].get<double>() * 100) + " |");
  }
  return lines;
}

std::vector<nlohmann::json> ChangedRows(const nlohmann::json& report) {
  const nlohmann::json& changed = GetOr(report, "changed_files", EmptyArray());
  std::vector<nlohmann::json> rows(changed.begin(), changed.end());
  const auto key = [](const nlohmann::json& row) {
    const nlohmann::json& delta = row["raw_delta"];
    const double magnitude =
        Truthy(delta) ? std::abs(delta.get<double>()) : 0.0;
    return std::tuple(delta.is_null() ? 1 : 0, -magnitude,
                      row["path"].get<std::string>());
  };
  std::ranges::stable_sort(
      rows, [&](const nlohmann::json& left, const nlohmann::json& right) {
        return key(left) < key(right);
      });
  return rows;
}

std::vector<std::string> ChangedTableLines(std::span<const nlohmann::json> rows,
                                           std::size_t total_head) {
  const std::string delta(kDelta);
  const std::string dash(kDash);
  std::vector<std::string> lines = {
      "| Path | Category | LM-CC base | LM-CC head | LM-CC " + delta +
          " | LM-CC " + delta + "% | Head rank |",
      "|---|---|---:|---:|---:|---:|---:|",
  };
  for (const nlohmann::json& row : rows) {
    const nlohmann::json& head = row["head"];
    const nlohmann::json& base = row["base"];
    const nlohmann::json& side =
        Truthy(head) ? head : (Truthy(base) ? base : EmptyObject());
    std::string rank = dash;
    if (Truthy(head) && !Get(head, "rank").is_null()) {
      rank = "#" + Integer(head["rank"]) + "/" + std::to_string(total_head);
    }
    const nlohmann::json& category = Get(side, "category");
    // An added or deleted path has no opposite side at all, which reads
    // differently from a present file whose score could not be measured.
    const bool both = !base.is_null() && !head.is_null();
    lines.push_back("| " + Code(row["path"].get<std::string>(), true) + " | " +
                    (Truthy(category) ? Str(category) : dash) + " | " +
                    (base.is_null() ? dash : Raw(base["llm_cc"])) + " | " +
                    (head.is_null() ? dash : Raw(head["llm_cc"])) + " | " +
                    (both ? Raw(row["raw_delta"], true) : dash) + " | " +
                    (both ? Percent(row["raw_percent"]) : dash) + " | " + rank +
                    " |");
  }
  return lines;
}

std::vector<Section> CommentSections(const nlohmann::json& report, bool full) {
  const nlohmann::json& identity = GetOr(report, "identity", EmptyObject());
  static const nlohmann::json kNoRankings = {{"base", nlohmann::json::array()},
                                             {"head", nlohmann::json::array()}};
  const nlohmann::json& rankings = GetOr(report, "rankings", kNoRankings);
  std::vector<Section> sections;
  std::vector<std::string> header = {
      "## llm-cc comparison",
      "",
      "Status: **" + Str(report["status"]) + "**",
      "",
      "Scores and ranks use total LM-CC; LM-CC/token is secondary.",
      ""};
  for (std::string& line : Headline(report)) {
    header.push_back(std::move(line));
  }
  sections.push_back({.priority = 1, .lines = std::move(header)});
  sections.push_back({.priority = 2, .lines = CategoryTable(report)});
  std::vector<std::string> status = {
      "", "Cache: " + Integer(report["cache_stats"]["hits"]) + " hits, " +
              Integer(report["cache_stats"]["misses"]) + " misses."};
  const nlohmann::json& errors = report["errors"];
  if (Truthy(errors)) {
    status.emplace_back("");
    status.emplace_back("Errors:");
    std::size_t count = 0;
    for (const nlohmann::json& error : errors) {
      if (count++ == 20) {
        break;
      }
      status.push_back("- " + Code(Str(error)));
    }
  }
  sections.push_back({.priority = 3, .lines = std::move(status)});

  const std::vector<nlohmann::json> rows = ChangedRows(report);
  const std::size_t total_head = GetOr(rankings, "head", EmptyArray()).size();
  if (!rows.empty()) {
    const std::size_t inline_count = std::min(rows.size(), kChangedFilesInline);
    std::vector<std::string> lines = {"", "### Changed files", ""};
    for (std::string& line :
         ChangedTableLines(std::span(rows).first(inline_count), total_head)) {
      lines.push_back(std::move(line));
    }
    if (!full && rows.size() > inline_count) {
      lines.emplace_back("");
      lines.push_back("_" + std::to_string(rows.size() - inline_count) +
                      " more changed files are listed in the full report._");
    }
    sections.push_back({.priority = 4, .lines = std::move(lines)});
  }

  for (const auto& [title, key] :
       {std::pair{"Leading regressions", "leading_regressions"},
        std::pair{"Leading improvements", "leading_improvements"}}) {
    const nlohmann::json& entries = GetOr(report, key, EmptyArray());
    if (!Truthy(entries)) {
      continue;
    }
    std::vector<std::string> lines = {"", std::string("### ") + title, ""};
    for (const nlohmann::json& entry : entries) {
      lines.push_back("- " + Code(Str(entry["path"])) + ": " +
                      PathChange(entry));
    }
    sections.push_back({.priority = 5, .lines = std::move(lines)});
  }

  const nlohmann::json& base_rankings = GetOr(rankings, "base", EmptyArray());
  if (!base_rankings.empty()) {
    // rankings["base"] is the merge base, not the target branch tip. Label
    // it as such: a branch behind its target would otherwise present stale
    // scores as the target branch's current state.
    const nlohmann::json& base = Get(identity, "base_sha");
    std::vector<std::string> lines = {
        "",
        "<details>",
        "<summary>Top offenders on the merge base (" +
            Code(Truthy(base) ? Str(base) : "base") + ")</summary>",
        "",
        "| # | Path | Category | LM-CC | Touched |",
        "|---:|---|---|---:|---|"};
    std::size_t count = 0;
    for (const nlohmann::json& entry : base_rankings) {
      if (count++ == 10) {
        break;
      }
      lines.push_back("| " + Integer(entry["rank"]) + " | " +
                      Code(Str(entry["path"]), true) + " | " +
                      Str(entry["category"]) + " | " + Raw(entry["llm_cc"]) +
                      " | " + (Truthy(entry["changed"]) ? "yes" : "") + " |");
    }
    lines.emplace_back("");
    lines.emplace_back("</details>");
    sections.push_back({.priority = 6, .lines = std::move(lines)});
  }

  const nlohmann::json& links =
      GetOr(GetOr(report, "presentation", EmptyObject()), "report_links",
            EmptyObject());
  const nlohmann::json& baseline = Get(links, "baseline");
  if (baseline.is_string() &&
      baseline.get_ref<const std::string&>().starts_with("https://")) {
    sections.push_back({.priority = 7,
                        .lines = {"", "Baseline ranking: <" +
                                          baseline.get<std::string>() + ">"}});
  }

  const nlohmann::json& source = GetOr(report, "rules_source", EmptyObject());
  const nlohmann::json& kind = Get(source, "source");
  if (kind == "repository") {
    const nlohmann::json& commit = Get(source, "commit");
    const std::string commit_text = Truthy(commit) ? Str(commit) : "";
    sections.push_back(
        {.priority = 8,
         .lines = {"", "Rules: repository " + Code(Str(Get(source, "path"))) +
                           "@" + commit_text.substr(0, 7)}});
  } else if (kind == "host") {
    sections.push_back({.priority = 8, .lines = {"", "Rules: host"}});
  } else if (kind == "builtin") {
    sections.push_back({.priority = 8, .lines = {"", "Rules: built-in"}});
  }
  return sections;
}

std::vector<std::string> FullSections(const nlohmann::json& report) {
  const std::vector<nlohmann::json> rows = ChangedRows(report);
  const std::size_t total_head =
      GetOr(GetOr(report, "rankings", EmptyObject()), "head", EmptyArray())
          .size();
  std::vector<std::string> lines;
  const auto append = [&lines](std::vector<std::string> more) {
    for (std::string& line : more) {
      lines.push_back(std::move(line));
    }
  };
  if (rows.size() > kChangedFilesInline) {
    append({"", "<details>",
            "<summary>All " + std::to_string(rows.size()) +
                " changed files</summary>",
            ""});
    append(ChangedTableLines(std::span(rows).subspan(kChangedFilesInline),
                             total_head));
    append({"", "</details>"});
  }
  append({"", "### Raw totals", "",
          "| Side/category | LM-CC | Tokens | LM-CC/token |",
          "|---|---:|---:|---:|"});
  for (const std::string_view side : {"base", "head"}) {
    for (const std::string_view name : kCategories) {
      const nlohmann::json& values =
          report["sides"][std::string(side)]["categories"][std::string(name)];
      lines.push_back("| " + std::string(side) + "/" + std::string(name) +
                      " | " + Raw(values["llm_cc"]) + " | " +
                      Fmt(values["token_count"]) + " | " +
                      Fmt(values["score"]) + " |");
    }
  }
  append({"", "### Coverage details", ""});
  for (const std::string_view side : {"base", "head"}) {
    const nlohmann::json& totals = report["sides"][std::string(side)]["totals"];
    std::string reasons;
    for (const auto& [reason, count] : totals["unmeasured_reasons"].items()) {
      reasons += (reasons.empty() ? "" : ", ") + reason + ": " + Integer(count);
    }
    lines.push_back("- " + Title(side) + ": " +
                    Integer(totals["measured_paths"]) + "/" +
                    Integer(totals["supported_paths"]) +
                    " supported paths measured; unscored paths: " +
                    (reasons.empty() ? "none" : reasons));
  }
  append({"", "### Changed paths", ""});
  if (Truthy(report["changes"])) {
    for (const nlohmann::json& change : report["changes"]) {
      const std::string old_path = change["old_path"].is_null()
                                       ? std::string(kEmptySet)
                                       : Code(Str(change["old_path"]));
      const std::string new_path = change["new_path"].is_null()
                                       ? std::string(kEmptySet)
                                       : Code(Str(change["new_path"]));
      lines.push_back("- " + old_path + " " + std::string(kArrow) + " " +
                      new_path + " (" + Str(change["status"]) + ")");
    }
  } else {
    lines.emplace_back("- No changed paths.");
  }
  for (const std::string_view side : {"base", "head"}) {
    append({"", "### " + Title(side) + " inventory", "",
            "| Path | Category | Language | Bytes | Measurement |",
            "|---|---|---|---:|---|"});
    for (const nlohmann::json& file :
         report["inventories"][std::string(side)]) {
      const nlohmann::json& key = Get(file, "key");
      const bool measured = Truthy(file["scorable"]) && key.is_string() &&
                            report["results"].contains(key.get<std::string>());
      const nlohmann::json& reason = file["reason"];
      const std::string measurement =
          measured ? "measured" : (Truthy(reason) ? Str(reason) : "missing");
      const nlohmann::json& language = file["language"];
      lines.push_back(
          "| " + Code(Str(file["path"]), true) + " | " + Str(file["category"]) +
          " | " + (Truthy(language) ? Str(language) : std::string(kDash)) +
          " | " +
          (file["size"].is_null() ? std::string(kDash) : Str(file["size"])) +
          " | " + measurement + " |");
    }
  }
  return lines;
}

std::vector<std::string> RankingTable(std::span<const nlohmann::json> entries) {
  std::vector<std::string> rows = {
      "| # | Path | Category | LM-CC | LM-CC/token | Tokens |",
      "|---:|---|---|---:|---:|---:|"};
  for (const nlohmann::json& entry : entries) {
    rows.push_back(
        "| " + Integer(entry["rank"]) + " | " + Code(Str(entry["path"]), true) +
        " | " + Str(entry["category"]) + " | " + Raw(entry["llm_cc"]) + " | " +
        Fmt(entry["score"]) + " | " + Integer(entry["token_count"]) + " |");
  }
  return rows;
}

}  // namespace

std::string RenderComment(const nlohmann::json& report) {
  return markdown::Assemble(CommentSections(report, false));
}

std::string RenderFullReport(const nlohmann::json& report) {
  std::vector<std::string> lines;
  for (Section& section : CommentSections(report, true)) {
    for (std::string& line : section.lines) {
      lines.push_back(std::move(line));
    }
  }
  for (std::string& line : FullSections(report)) {
    lines.push_back(std::move(line));
  }
  std::string text = Join(lines);
  while (!text.empty() && text.back() == '\n') {
    text.pop_back();
  }
  return text + "\n";
}

std::string RenderBaseline(const nlohmann::json& report) {
  const nlohmann::json& identity = GetOr(report, "identity", EmptyObject());
  const nlohmann::json& rankings =
      GetOr(GetOr(report, "rankings", EmptyObject()), "head", EmptyArray());
  const nlohmann::json& categories = report["sides"]["head"]["categories"];
  const auto known = [&](std::string_view key) {
    const nlohmann::json& value = Get(identity, key);
    return Truthy(value) ? Str(value) : std::string("unknown");
  };
  std::vector<std::string> lines = {
      "Baseline ranking for " + known("repository") + "@" + known("head_sha") +
          " (" + known("target_branch") + ")",
      "",
      "Status: **" + Str(report["status"]) + "**",
      "",
      "| Category | LM-CC | LM-CC/token | Tokens | Measured paths |",
      "|---|---:|---:|---:|---:|"};
  for (const std::string_view name : kCategories) {
    const nlohmann::json& values = categories[std::string(name)];
    lines.push_back("| " + std::string(name) + " | " + Raw(values["llm_cc"]) +
                    " | " + Fmt(values["score"]) + " | " +
                    Fmt(values["token_count"]) + " | " +
                    Integer(values["measured_paths"]) + "/" +
                    Integer(values["supported_paths"]) + " |");
  }
  const nlohmann::json& totals = report["sides"]["head"]["totals"];
  lines.push_back("| repository | " + Raw(totals["llm_cc"]) + " | " +
                  Fmt(totals["score"]) + " | " + Fmt(totals["token_count"]) +
                  " | " + Integer(totals["measured_paths"]) + "/" +
                  Integer(totals["supported_paths"]) + " |");
  const std::vector<nlohmann::json> ranked(rankings.begin(), rankings.end());
  const auto append = [&lines](std::vector<std::string> more) {
    for (std::string& line : more) {
      lines.push_back(std::move(line));
    }
  };
  append({"", "## Top 50 files", ""});
  if (ranked.empty()) {
    lines.emplace_back("No measured files.");
  } else {
    append(RankingTable(
        std::span(ranked).first(std::min<std::size_t>(ranked.size(), 50))));
  }
  for (const std::string_view name : kCategories) {
    std::vector<nlohmann::json> selected;
    for (const nlohmann::json& entry : ranked) {
      if (entry["category"] == name && selected.size() < 20) {
        selected.push_back(entry);
      }
    }
    append({"", "## Top 20 " + std::string(name) + " files", ""});
    if (selected.empty()) {
      lines.emplace_back("No measured files.");
    } else {
      append(RankingTable(selected));
    }
  }
  append({"", "## Unmeasured paths", ""});
  const nlohmann::json& reasons = totals["unmeasured_reasons"];
  if (Truthy(reasons)) {
    for (const auto& [reason, count] : reasons.items()) {
      lines.push_back("- " + reason + ": " + Integer(count));
    }
  } else {
    lines.emplace_back("- None.");
  }
  return Join(lines) + "\n";
}

nlohmann::json BaselineJson(const nlohmann::json& report) {
  return {{"schema_version", report.value("schema_version", 1)},
          {"identity", report["identity"]},
          {"fingerprint", report["fingerprint"]},
          {"status", report["status"]},
          {"categories", report["sides"]["head"]["categories"]},
          {"rankings", GetOr(GetOr(report, "rankings", EmptyObject()), "head",
                             EmptyArray())}};
}

void WriteReportArtifacts(const std::filesystem::path& output,
                          const nlohmann::json& report,
                          const std::string& report_markdown,
                          const std::string& comment) {
  WriteJsonFile(output / "report.json", report);
  WriteFileAtomic(output / "report.md", report_markdown);
  WriteFileAtomic(output / "baseline.md", RenderBaseline(report));
  WriteJsonFile(output / "baseline.json", BaselineJson(report));
  WriteFileAtomic(output / "comment.md", comment);
  WriteJsonFile(output / "publication.json",
                {{"schema_version", report.value("schema_version", 1)},
                 {"identity", report["identity"]},
                 {"fingerprint", report["fingerprint"]},
                 {"status", report["status"]},
                 {"comment",
                  {{"path", "comment.md"},
                   {"sha256", Sha256Hex(comment)},
                   {"bytes", comment.size()}}}});
}

}  // namespace llmcc::compare
