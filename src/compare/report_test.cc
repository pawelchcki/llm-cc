// Report behavior ported from the Python comparison tests.
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "src/compare/aggregate.h"
#include "src/compare/json_util.h"
#include "src/compare/markdown.h"
#include "src/compare/plan.h"
#include "src/compare/report_model.h"
#include "src/compare/report_render.h"
#include "src/test_util.h"

namespace {

using llmcc::compare::Delta;
using llmcc::compare::markdown::Assemble;
using llmcc::compare::markdown::Code;
using llmcc::compare::markdown::Section;
using llmcc::test::Expect;
using llmcc::test::ExpectEq;
using nlohmann::json;

bool Contains(std::string_view text, std::string_view fragment) {
  return text.find(fragment) != std::string_view::npos;
}

std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  for (std::string line; std::getline(stream, line);) {
    lines.push_back(line);
  }
  return lines;
}

void ExpectBalancedFences(const std::string& markdown) {
  for (const std::string& line : Lines(markdown)) {
    Expect(std::ranges::count(line, '`') % 2 == 0,
           "code fences are balanced in " + line);
  }
}

json Report(const json& overrides = json::object()) {
  const json comparison = {{"score", Delta(1.0, 2.0)},
                           {"raw_llm_cc", Delta(1.0, 2.0)},
                           {"tokens", Delta(1, 1)},
                           {"coverage", {{"base", 1.0}, {"head", 1.0}}}};
  json report = {
      {"status", "complete"},
      {"identity", {{"target_branch", "main"}, {"repository", "o/r"}}},
      {"comparisons",
       {{"runtime", comparison},
        {"tests", comparison},
        {"tooling", comparison},
        {"repository", comparison}}},
      {"cache_stats", {{"hits", 1}, {"misses", 0}}},
      {"rankings", {{"base", json::array()}, {"head", json::array()}}},
      {"changed_files", json::array()},
      {"leading_regressions", json::array()},
      {"leading_improvements", json::array()},
      {"rules_source", {{"source", "host"}}},
      {"presentation", json::object()},
      {"errors", json::array()}};
  report.update(overrides);
  return report;
}

json ChangedRow(const std::string& path, int index) {
  return {{"status", "M"},
          {"old_path", path},
          {"new_path", path},
          {"path", path},
          {"base", {{"category", "runtime"}, {"score", 1.0}, {"llm_cc", 1.0}}},
          {"head",
           {{"category", "runtime"},
            {"score", 2.0 + index},
            {"llm_cc", 2.0 + index},
            {"rank", index + 1},
            {"category_rank", index + 1}}},
          {"delta", 1.0 + index},
          {"percent", 100.0},
          {"raw_delta", 1.0 + index},
          {"raw_percent", 100.0}};
}

json File(const std::string& path, const std::string& key) {
  return {{"path", path},     {"key", key},  {"category", "tests"},
          {"language", "c"},  {"size", 100}, {"scorable", true},
          {"reason", nullptr}};
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  // Control and format characters become visible escapes.
  const std::string neutral = llmcc::compare::markdown::NeutralizeControls(
      "src/@team/[file]<tag>\xE2\x80\xAE.cc");
  Expect(!Contains(neutral, "\xE2\x80\xAE") && Contains(neutral, "\\u202e"),
         "a right-to-left override is escaped");

  // Raw LM-CC is the headline; per-token deltas are secondary.
  const std::string headline = llmcc::compare::RenderComment(Report(
      {{"leading_regressions",
        json::array({{{"path", "a.cc"}, {"change", 1.0}, {"raw_change", 2.5}},
                     {{"path", "legacy.cc"}, {"change", 0.5}},
                     {{"path", "empty.cc"},
                      {"change", nullptr},
                      {"raw_change", 3.0}}})}}));
  Expect(Contains(headline, "- `empty.cc`: +3.0 LM-CC\n"),
         "zero-token paths report raw LM-CC only");
  Expect(Contains(headline, "- repository 1.0 \xE2\x86\x92 2.0 LM-CC (+100%)"),
         "the headline shows raw LM-CC");
  Expect(Contains(headline, "| repository | 1.0 | 2.0 | +1.0 | +100% |"),
         "the category table leads with raw LM-CC");
  Expect(Contains(headline, "- `a.cc`: +2.5 LM-CC (+1 LM-CC/token)"),
         "per-token deltas follow raw deltas");
  Expect(Contains(headline, "- `legacy.cc`: +0.5 LM-CC/token"),
         "reports before raw headlines still render");

  // Untrusted paths stay inside balanced code spans with escaped pipes.
  const std::string hostile =
      "src/@team/[file]`name``x`~~$y$|pipe|https://example.com/a.cc";
  json offenders = json::array();
  for (int index = 0; index < 130; ++index) {
    offenders.push_back({{"path", hostile}, {"rank", 3}, {"category_rank", 2}});
  }
  const std::string untrusted = llmcc::compare::RenderComment(
      Report({{"leading_regressions",
               json::array({{{"path", hostile}, {"change", 1.0}}})},
              {"changed_files", json::array({ChangedRow(hostile, 2)})},
              {"rankings", {{"base", json::array()}, {"head", offenders}}}}));
  ExpectBalancedFences(untrusted);
  const auto lines = Lines(untrusted);
  const auto row = std::ranges::find_if(
      lines, [](const std::string& line) { return line.starts_with("| ``"); });
  Expect(row != lines.end() && Contains(*row, "\\|pipe\\|") &&
             Contains(*row, "#3/130"),
         "table cells escape pipes and show the head rank");
  const auto bullet = std::ranges::find_if(lines, [](const std::string& line) {
    return line.starts_with("- ``") && Contains(line, "@team");
  });
  Expect(bullet != lines.end() && Contains(*bullet, "[file]") &&
             Contains(*bullet, "https://example.com"),
         "bullets keep the whole untrusted path inside code");

  // Code spans fence backticks and shorten long paths.
  ExpectEq(Code("plain"), std::string("`plain`"), "plain text is one span");
  ExpectEq(Code("a`b"), std::string("``a`b``"), "backticks lengthen the fence");
  ExpectEq(Code("`lead"), std::string("`` `lead ``"),
           "a leading backtick is padded");
  ExpectEq(Code("a``b`"), std::string("``` a``b` ```"),
           "the fence outgrows every run");
  const std::string shortened = Code("d/" + std::string(300, 'x') + ".py");
  Expect(Contains(shortened, "\xE2\x80\xA6") && shortened.starts_with("`d/xxx"),
         "long paths keep their start around an ellipsis");
  ExpectEq(Code("a|b", true), std::string("`a\\|b`"),
           "table pipes are escaped");
  ExpectEq(Code("a\\|@team.cc", true), std::string("`a\\\\\\|@team.cc`"),
           "a backslash before a pipe is doubled");
  ExpectEq(Code("odd\\u000aname.py", true), std::string("`odd\\u000aname.py`"),
           "other backslashes stay literal");
  for (const std::string path : {"a|b", "a\\|b", "a\\\\|b"}) {
    const std::string cell = Code(path, true);
    for (std::size_t index = 0; index < cell.size(); ++index) {
      if (cell[index] != '|') {
        continue;
      }
      std::size_t run = 0;
      while (run < index && cell[index - run - 1] == '\\') {
        ++run;
      }
      Expect(run % 2 == 1, "every pipe is escaped in " + cell);
    }
  }

  // Links and repository rules are announced.
  const std::string announced = llmcc::compare::RenderComment(
      Report({{"presentation",
               {{"report_links",
                 {{"baseline", "https://ci.example/o/r/baseline.md"}}}}},
              {"rules_source",
               {{"source", "repository"},
                {"path", ".llm-cc/comparison-rules.json"},
                {"commit", "0123456789abcdef"}}}}));
  Expect(Contains(announced,
                  "Baseline ranking: <https://ci.example/o/r/baseline.md>") &&
             !Contains(announced, "]("),
         "the baseline link is an autolink");
  Expect(Contains(announced,
                  "Rules: repository `.llm-cc/comparison-rules.json`@0123456"),
         "repository rules name their commit");
  Expect(Contains(llmcc::compare::RenderComment(Report()), "Rules: host"),
         "host rules are announced");
  Expect(Contains(llmcc::compare::RenderComment(
                      Report({{"rules_source", {{"source", "builtin"}}}})),
                  "Rules: built-in"),
         "built-in rules are announced");
  Expect(!Contains(llmcc::compare::RenderComment(Report(
                       {{"presentation",
                         {{"report_links",
                           {{"baseline", "http://insecure.example"}}}}}})),
                   "Baseline ranking"),
         "only https links are published");

  // The comment stays within its limit however many files change.
  json rows = json::array();
  json ranked = json::array();
  for (int index = 0; index < 3000; ++index) {
    const std::string path =
        "src/" + std::string(80, 'd') + "/" + std::to_string(index) + ".cc";
    rows.push_back(ChangedRow(path, index));
    ranked.push_back({{"path", path},
                      {"category", "runtime"},
                      {"llm_cc", 1.0},
                      {"rank", index + 1},
                      {"changed", true}});
  }
  const std::string limited = llmcc::compare::RenderComment(
      Report({{"changed_files", rows},
              {"rankings", {{"base", ranked}, {"head", ranked}}},
              {"errors", json::array({std::string(2000, 'e')})}}));
  Expect(limited.size() <= llmcc::compare::markdown::kCommentLimit,
         "the comment fits the publication limit");
  ExpectBalancedFences(limited);
  Expect(Contains(limited, "### Changed files") &&
             Contains(limited,
                      "_2975 more changed files are listed in the full "
                      "report._"),
         "the comment points to the full report");

  // Assembly drops whole sections by priority, then whole lines.
  std::vector<Section> sections = {
      {.priority = 1,
       .lines = {"## llm-cc comparison", "", "Status: **complete**"}},
      {.priority = 4, .lines = {"", "### Changed files"}},
      {.priority = 6, .lines = {"", "### Top offenders on base"}}};
  for (int index = 0; index < 30; ++index) {
    sections[1].lines.push_back("| " + Code(std::string(100, 'a')) + " |");
  }
  for (int index = 0; index < 200; ++index) {
    sections[2].lines.emplace_back("- x");
  }
  const std::string dropped = Assemble(sections, 4096);
  Expect(dropped.size() <= 4096 && Contains(dropped, "### Changed files") &&
             !Contains(dropped, "Top offenders"),
         "the least important section is dropped first");
  std::vector<std::string> crowded_lines = {"## llm-cc comparison"};
  for (int index = 0; index < 200; ++index) {
    crowded_lines.push_back("- " + Code(std::string(100, 'b')));
  }
  const std::string crowded =
      Assemble({{.priority = 1, .lines = crowded_lines}}, 1024);
  Expect(crowded.size() <= 1024 &&
             crowded.ends_with("_Full details are available in artifacts._\n"),
         "a lone oversized section is cut on a line boundary");
  ExpectBalancedFences(crowded);

  // Deltas are undefined without both sides or a nonzero base.
  const json zero_base = Delta(0.0, 1.0);
  Expect(zero_base["percent"].is_null() && zero_base["absolute"] == 1.0,
         "a zero base has no percentage");
  Expect(Delta(nullptr, 1.0)["absolute"].is_null(),
         "a missing side has no delta");
  Expect(Delta(3, 5)["absolute"].is_number_integer() &&
             Delta(3, 5.0)["absolute"].is_number_float(),
         "integer deltas stay integers");

  // Rankings and changed files follow total LM-CC, not density.
  const json results = {{"base", {{"llm_cc", 48.2}, {"token_count", 1431}}},
                        {"head", {{"llm_cc", 45.4}, {"token_count", 663}}},
                        {"small", {{"llm_cc", 2.0}, {"token_count", 2}}},
                        {"large", {{"llm_cc", 100.0}, {"token_count", 10000}}},
                        {"empty", {{"llm_cc", 0.0}, {"token_count", 0}}}};
  const json base_inventory =
      json::array({File("test_url_parse.c", "base"), File("small.c", "small"),
                   File("large.c", "large"), File("empty.c", "empty")});
  const json head_inventory =
      json::array({File("test_url_parse.c", "head"), File("small.c", "small"),
                   File("large.c", "large"), File("empty.c", "empty")});
  const std::set<std::string> changed = {"test_url_parse.c"};
  const json head_rankings =
      llmcc::compare::Rankings(head_inventory, results, changed);
  std::vector<std::string> order;
  for (const json& entry : head_rankings) {
    order.push_back(entry["path"]);
  }
  ExpectEq(order,
           std::vector<std::string>{"large.c", "test_url_parse.c", "small.c",
                                    "empty.c"},
           "rankings order by total LM-CC");
  Expect(head_rankings.back()["score"].is_null() &&
             head_rankings[1]["changed"] == true &&
             head_rankings[0]["category_rank"] == 1,
         "rankings carry per-token scores, ranks and changes");
  const json change = {{"status", "M"},
                       {"old_path", "test_url_parse.c"},
                       {"new_path", "test_url_parse.c"}};
  const json changed_rows =
      llmcc::compare::ChangedFiles(json::array({change}), base_inventory,
                                   head_inventory, results, head_rankings);
  Expect(std::abs(changed_rows[0]["raw_delta"].get<double>() + 2.8) < 1e-9 &&
             changed_rows[0]["delta"].get<double>() > 0,
         "raw LM-CC falls while density rises");
  const std::string density = llmcc::compare::RenderComment(Report(
      {{"changed_files", changed_rows},
       {"rankings",
        {{"base", llmcc::compare::Rankings(base_inventory, results, changed)},
         {"head", head_rankings}}}}));
  Expect(Contains(density,
                  "| `test_url_parse.c` | tests | 48.2 | 45.4 | "
                  "-2.8 | -5.81% | #2/4 |") &&
             Contains(density, "| 1 | `large.c` | tests | 100.0 |"),
         "file tables follow total LM-CC");
  const json missing_side = llmcc::compare::ChangedFiles(
      json::array(
          {{{"status", "M"}, {"old_path", "base"}, {"new_path", "head"}}}),
      json::array({File("base", "base")}), json::array({File("head", "head")}),
      json{{"head", {{"llm_cc", 0.0}, {"token_count", 0}}}}, json::array());
  Expect(missing_side[0]["base"]["llm_cc"].is_null() &&
             missing_side[0]["head"]["llm_cc"] == 0.0 &&
             missing_side[0]["raw_delta"].is_null(),
         "an unmeasured side leaves the raw delta undefined");

  // Leading paths rank by raw LM-CC, including zero-token and added files.
  llmcc::compare::ReportInputs inputs{
      .identity = {{"repository", "o/r"}},
      .fingerprint = "f",
      .inventories = {{"base", json::array({File("same.cc", "same-base"),
                                            File("gone.cc", "gone")})},
                      {"head", json::array({File("same.cc", "same-head"),
                                            File("added.cc", "added")})}},
      .changes = json::array(
          {{{"status", "M"}, {"old_path", "same.cc"}, {"new_path", "same.cc"}},
           {{"status", "D"}, {"old_path", "gone.cc"}, {"new_path", nullptr}},
           {{"status", "A"}, {"old_path", nullptr}, {"new_path", "added.cc"}}}),
      .results = {{"same-base", {{"llm_cc", 3.0}, {"token_count", 0}}},
                  {"same-head", {{"llm_cc", 5.5}, {"token_count", 2}}},
                  {"gone", {{"llm_cc", 1.25}, {"token_count", 5}}},
                  {"added", {{"llm_cc", 4}, {"token_count", 2}}}},
      .rules_source = nullptr,
      .presentation = nullptr,
      .cache_stats = {{"hits", 0}, {"misses", 0}}};
  const json built = llmcc::compare::BuildReport(inputs);
  const json& regressions = built["leading_regressions"];
  ExpectEq(regressions.size(), std::size_t{2}, "two paths regress");
  Expect(regressions[0]["path"] == "added.cc" &&
             regressions[0]["raw_change"] == 4.0 &&
             regressions[0]["change"].is_null(),
         "an added file carries its whole raw score");
  Expect(regressions[1]["path"] == "same.cc" &&
             regressions[1]["raw_change"] == 2.5 &&
             regressions[1]["change"].is_null(),
         "a zero-token side keeps the raw delta");
  Expect(built["leading_improvements"][0]["path"] == "gone.cc" &&
             built["leading_improvements"][0]["raw_change"] == -1.25,
         "a deleted file improves by its whole score");
  Expect(built["status"] == "complete" &&
             built["change_counts"]["additions"] == 1 &&
             built["change_counts"]["deletions"] == 1,
         "changes are counted");

  // An unreadable plan still yields a complete failure report set.
  const char* temporary = std::getenv("TEST_TMPDIR");
  Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const std::filesystem::path root(temporary);
  std::ofstream(root / "plan.json") << "{bad";
  const json malformed = llmcc::compare::AggregatePlan(
      root / "plan.json", {"missing-worker.json"}, root / "malformed");
  Expect(malformed["status"] == "failed" &&
             std::filesystem::exists(root / "malformed/comment.md") &&
             std::filesystem::exists(root / "malformed/publication.json") &&
             Contains(malformed["errors"][0].get<std::string>(),
                      "aggregation failed"),
         "a malformed plan becomes a failure report");
  const json failure = llmcc::compare::WriteFailureReport(
      root / "failure", {{"repository", "o/r"}, {"extra", 1}}, nullptr,
      {"boom"});
  Expect(failure["identity"].size() == 8 &&
             failure["identity"]["repository"] == "o/r" &&
             !failure["identity"].contains("extra"),
         "failure identities are normalized");

  // Counts beyond any file's size never reach the signed report totals.
  const json item = {{"key", "k"},
                     {"blob_id", std::string(40, 'b')},
                     {"language", "cpp"},
                     {"size", 3}};
  const json valid = llmcc::compare::ResultRecord(
      item, "f", {.llm_cc = 1.5, .token_count = 3});
  Expect(llmcc::compare::ValidResult(valid, item, "f"),
         "a plausible result is valid");
  for (const char* field : {"token_count", "high_entropy_tokens",
                            "total_branch", "total_comp_level"}) {
    json oversized = valid;
    oversized[field] = json::parse("18446744073709551615");
    Expect(!llmcc::compare::ValidResult(oversized, item, "f"),
           std::string("a count past the source limit is invalid: ") + field);
  }
  json huge_score = valid;
  huge_score["llm_cc"] = json::parse("18446744073709551615");
  Expect(!llmcc::compare::ValidResult(huge_score, item, "f"),
         "a score past 2^53 is invalid");
  return 0;
}
