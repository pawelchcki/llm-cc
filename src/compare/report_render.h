#ifndef LLM_CC_COMPARE_REPORT_RENDER_H_
#define LLM_CC_COMPARE_REPORT_RENDER_H_

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace llmcc::compare {

// The pull-request comment: prioritized sections within the comment limit.
std::string RenderComment(const nlohmann::json& report);
// report.md: every comment section in full, then totals, coverage, changes
// and both inventories.
std::string RenderFullReport(const nlohmann::json& report);
// baseline.md: the head revision's rankings on their own.
std::string RenderBaseline(const nlohmann::json& report);
nlohmann::json BaselineJson(const nlohmann::json& report);

// Writes report.json, report.md, baseline.md, baseline.json, comment.md and
// last publication.json, whose presence marks the set complete.
void WriteReportArtifacts(const std::filesystem::path& output,
                          const nlohmann::json& report,
                          const std::string& report_markdown,
                          const std::string& comment);

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_REPORT_RENDER_H_
