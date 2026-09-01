#ifndef LLM_CC_LANG_H_
#define LLM_CC_LANG_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/core.h"

namespace llmcc {

enum class Language : std::uint8_t { kRust, kC, kCpp };
using OffsetMap = std::vector<std::size_t>;

std::pair<std::string, OffsetMap> StripComments(std::string_view source,
                                                Language language);
std::vector<StructuralEvent> StructuralEvents(std::string_view source,
                                              Language language);
Language ParseLanguage(std::string_view name);
Language InferLanguage(std::string_view path);
std::string_view LanguageName(Language language);
bool IsHeaderPath(std::string_view path);
bool IsSourcePath(std::string_view path, bool include_headers);

}  // namespace llmcc

#endif  // LLM_CC_LANG_H_
