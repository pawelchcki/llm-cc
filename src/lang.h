#ifndef LLM_CC_LANG_H_
#define LLM_CC_LANG_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/core.h"
#include "src/offset_map.h"

namespace llmcc {

enum class Language : std::uint8_t {
  kRust,
  kC,
  kCpp,
  kJava,
  kPython,
  kGo,
  kJavaScript,
  kCSharp,
};
struct PreprocessOptions {
  std::chrono::milliseconds time_budget = std::chrono::seconds(300);
  std::size_t max_syntax_depth = 1024;
};

struct FunctionSpan {
  std::string name;
  std::size_t start_byte;
  std::size_t end_byte;
  bool operator==(const FunctionSpan&) const = default;
};

// All source-derived data used by analysis.  The original text is parsed only
// once; ranges collected from that tree are remapped after comments and Python
// docstrings are removed.
struct PreparedSource {
  std::string cleaned;
  OffsetMap original_offsets;
  std::vector<std::size_t> line_starts;
  std::vector<SourceRange> meaningful_ranges;
  std::vector<StructuralEvent> structural_events;
  std::vector<FunctionSpan> functions;
};

PreparedSource PrepareSource(std::string_view source, Language language,
                             PreprocessOptions options = {});

std::pair<std::string, OffsetMap> StripComments(std::string_view source,
                                                Language language,
                                                PreprocessOptions options = {});
std::vector<std::size_t> LineStarts(std::string_view source);
std::vector<StructuralEvent> StructuralEvents(std::string_view source,
                                              Language language,
                                              PreprocessOptions options = {});
std::vector<FunctionSpan> Functions(std::string_view preprocessed,
                                    Language language,
                                    PreprocessOptions options = {});
Language ParseLanguage(std::string_view name);
Language InferLanguage(std::string_view path);
std::string_view LanguageName(Language language);
bool IsHeaderPath(std::string_view path);
bool IsSourcePath(std::string_view path, bool include_headers);

}  // namespace llmcc

#endif  // LLM_CC_LANG_H_
