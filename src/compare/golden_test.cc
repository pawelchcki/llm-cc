// Byte-for-byte agreement with the Python renderer the C++ port replaced.
// With LLM_CC_UPDATE_GOLDENS naming the source golden directory, run under
// --spawn_strategy=local, mismatching expectations are rewritten instead.
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "src/compare/aggregate.h"
#include "src/compare/json_util.h"
#include "src/compare/markdown.h"
#include "src/compare/report_render.h"
#include "src/test_util.h"

namespace {

namespace fs = std::filesystem;
using llmcc::compare::ReadFileBytes;
using llmcc::compare::ReadJsonFile;
using llmcc::test::Expect;

constexpr const char* kReportFiles[] = {  // NOLINT(modernize-avoid-c-arrays)
    "report.json", "report.md",     "comment.md",
    "baseline.md", "baseline.json", "publication.json"};

int failures = 0;

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void CompareFiles(const fs::path& expected, const fs::path& actual,
                  const std::string& label) {
  const char* update = std::getenv("LLM_CC_UPDATE_GOLDENS");
  for (const char* name : kReportFiles) {
    const std::string want = ReadFileBytes(expected / name);
    const std::string got = ReadFileBytes(actual / name);
    if (update != nullptr && want != got) {
      llmcc::compare::WriteFileAtomic(
          fs::path(update) / label / "expected" / name, got);
      continue;
    }
    Check(want == got, label + "/" + name + " matches the Python renderer");
  }
}

std::string FromHex(const std::string& hex) {
  std::string bytes;
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
    bytes.push_back(
        static_cast<char>(std::stoi(hex.substr(index, 2), nullptr, 16)));
  }
  return bytes;
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape)
  const char* test_srcdir = std::getenv("TEST_SRCDIR");
  const char* test_tmpdir = std::getenv("TEST_TMPDIR");
  Expect(argc == 2 && test_srcdir != nullptr && test_tmpdir != nullptr,
         "golden root and test directories are provided");
  const fs::path root = (fs::path(test_srcdir) / argv[1]).parent_path();
  const fs::path scratch = fs::path(test_tmpdir) / "golden";
  const fs::path original = fs::current_path();

  std::size_t cases = 0;
  for (const auto& entry : fs::directory_iterator(root / "aggregate")) {
    const nlohmann::json description = ReadJsonFile(entry.path() / "case.json");
    const std::vector<std::string> workers = description["workers"];
    const fs::path output = scratch / "aggregate" / entry.path().filename();
    fs::current_path(entry.path() / "input");
    llmcc::compare::AggregatePlan("plan.json", workers, output);
    fs::current_path(original);
    CompareFiles(entry.path() / "expected", output,
                 "aggregate/" + entry.path().filename().string());
    ++cases;
  }
  Check(cases >= 10, "aggregate goldens are present");

  for (const auto& entry : fs::directory_iterator(root / "failure")) {
    const nlohmann::json description = ReadJsonFile(entry.path() / "case.json");
    const fs::path output = scratch / "failure" / entry.path().filename();
    llmcc::compare::WriteFailureReport(
        output, description["identity"], description["fingerprint"],
        description["errors"].get<std::vector<std::string>>());
    CompareFiles(entry.path() / "expected", output,
                 "failure/" + entry.path().filename().string());
  }

  for (const auto& entry : fs::directory_iterator(root / "render")) {
    if (entry.path().extension() != ".json") {
      continue;
    }
    const nlohmann::json report = ReadJsonFile(entry.path());
    fs::path comment = entry.path();
    comment.replace_extension(".comment.md");
    Check(llmcc::compare::RenderComment(report) == ReadFileBytes(comment),
          "render/" + entry.path().filename().string() + " comment matches");
  }

  for (const nlohmann::json& row : ReadJsonFile(root / "code_spans.json")) {
    const std::string text = row[0];
    const std::string expected = row[2];
    Check(llmcc::compare::markdown::Code(text, row[1].get<bool>()) == expected,
          "code span of " + nlohmann::json(text).dump() + " matches");
  }

  for (const nlohmann::json& row : ReadJsonFile(root / "assemble.json")) {
    std::vector<llmcc::compare::markdown::Section> sections;
    for (const nlohmann::json& section : row["sections"]) {
      sections.push_back({.priority = section[0].get<int>(),
                          .lines = section[1].get<std::vector<std::string>>()});
    }
    Check(llmcc::compare::markdown::Assemble(sections,
                                             row["limit"].get<std::size_t>()) ==
              FromHex(row["expected_bytes"].get<std::string>()),
          "assembly within " + row["limit"].dump() + " bytes matches");
  }
  return failures == 0 ? 0 : 1;
}
