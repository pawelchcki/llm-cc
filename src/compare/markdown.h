#ifndef LLM_CC_COMPARE_MARKDOWN_H_
#define LLM_CC_COMPARE_MARKDOWN_H_

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace llmcc::compare::markdown {

// A pull-request comment must fit GitHub's limits with room to spare.
inline constexpr std::size_t kCommentLimit = std::size_t{24} * 1024;
// Longer paths keep their start and end around an ellipsis.
inline constexpr std::size_t kPathDisplayLimit = 120;

// Replaces control characters, format characters (bidirectional overrides,
// zero-width joiners) and line separators with a visible \uXXXX, so no
// untrusted text can reorder or hide what a comment shows. Bytes that are not
// UTF-8 become \udcXX.
std::string NeutralizeControls(std::string_view text);

// Untrusted text as one balanced code span Markdown cannot escape from. In a
// table, pipes are escaped because GFM splits cells on them even inside code.
std::string Code(std::string_view text, bool table = false);

struct Section {
  int priority = 1;
  std::vector<std::string> lines;
};

// Joins sections, then drops the least important ones, and finally whole
// lines, until the text fits `limit` bytes. A cut ends with a notice.
std::string Assemble(const std::vector<Section>& sections,
                     std::size_t limit = kCommentLimit);

// Number formats; JSON null renders as "unavailable".
std::string Fmt(const nlohmann::json& value);                     // %.6g
std::string Raw(const nlohmann::json& value, bool sign = false);  // %.1f
std::string Percent(const nlohmann::json& value);                 // %+.3g%

}  // namespace llmcc::compare::markdown

#endif  // LLM_CC_COMPARE_MARKDOWN_H_
