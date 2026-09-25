#include "src/glob.h"

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace llmcc {
namespace {

constexpr std::string_view kGlobstar = "**";

// Decodes one UTF-8 code point and advances `index` past it. A byte that does
// not start a valid sequence stands for itself, so arbitrary Git path bytes
// still match literally.
char32_t NextCodePoint(std::string_view text, std::size_t& index) {
  const auto lead = static_cast<unsigned char>(text[index]);
  std::size_t length = 1;
  char32_t value = lead;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
    value = lead & 0x1FU;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    value = lead & 0x0FU;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    value = lead & 0x07U;
  }
  if (length == 1 || index + length > text.size()) {
    ++index;
    return lead;
  }
  for (std::size_t offset = 1; offset < length; ++offset) {
    const auto next = static_cast<unsigned char>(text[index + offset]);
    if ((next & 0xC0U) != 0x80U) {
      ++index;
      return lead;
    }
    value = (value << 6U) | (next & 0x3FU);
  }
  // Overlong forms, surrogates and values past U+10FFFF are not characters:
  // each of their bytes stands for itself, as Python's surrogateescape reads
  // them.
  if ((length == 3 &&
       (value < 0x800U || (value >= 0xD800U && value <= 0xDFFFU))) ||
      (length == 4 && (value < 0x10000U || value > 0x10FFFFU))) {
    ++index;
    return lead;
  }
  index += length;
  return value;
}

// Matches a bracket expression at pattern[index] == '[' and advances `index`
// past its closing ']'. Compile() has already rejected unterminated sets.
bool MatchClass(std::string_view pattern, std::size_t& index, char32_t value) {
  ++index;
  bool negated = false;
  if (index < pattern.size() &&
      (pattern[index] == '!' || pattern[index] == '^')) {
    negated = true;
    ++index;
  }
  bool matched = false;
  bool first = true;
  while (index < pattern.size() && (first || pattern[index] != ']')) {
    first = false;
    if (pattern[index] == '\\') {
      ++index;
    }
    const char32_t low = NextCodePoint(pattern, index);
    char32_t high = low;
    if (index + 1 < pattern.size() && pattern[index] == '-' &&
        pattern[index + 1] != ']') {
      ++index;
      if (pattern[index] == '\\') {
        ++index;
      }
      high = NextCodePoint(pattern, index);
    }
    matched = matched || (low <= value && value <= high);
  }
  ++index;
  return matched != negated;
}

// Matches one non-star pattern element and advances both positions.
bool MatchOne(std::string_view pattern, std::size_t& pattern_index,
              std::string_view text, std::size_t& text_index) {
  const char token = pattern[pattern_index];
  const char32_t value = NextCodePoint(text, text_index);
  if (token == '?') {
    ++pattern_index;
    return true;
  }
  if (token == '[') {
    return MatchClass(pattern, pattern_index, value);
  }
  if (token == '\\') {
    ++pattern_index;
  }
  return NextCodePoint(pattern, pattern_index) == value;
}

bool MatchSegment(std::string_view pattern, std::string_view text) {
  std::size_t pattern_index = 0;
  std::size_t text_index = 0;
  std::size_t star_pattern = std::string_view::npos;
  std::size_t star_text = 0;
  while (text_index < text.size()) {
    if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
      while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
        ++pattern_index;
      }
      star_pattern = pattern_index;
      star_text = text_index;
      continue;
    }
    std::size_t next_pattern = pattern_index;
    std::size_t next_text = text_index;
    if (pattern_index < pattern.size() &&
        MatchOne(pattern, next_pattern, text, next_text)) {
      pattern_index = next_pattern;
      text_index = next_text;
      continue;
    }
    if (star_pattern == std::string_view::npos) {
      return false;
    }
    // Let the last star absorb one more character and retry after it.
    NextCodePoint(text, star_text);
    text_index = star_text;
    pattern_index = star_pattern;
  }
  while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
    ++pattern_index;
  }
  return pattern_index == pattern.size();
}

// Segment-level wildcard matching where "**" absorbs any run of segments.
bool MatchSegments(std::span<const std::string> pattern,
                   std::span<const std::string_view> path) {
  std::size_t pattern_index = 0;
  std::size_t path_index = 0;
  std::size_t star_pattern = std::string_view::npos;
  std::size_t star_path = 0;
  while (path_index < path.size()) {
    if (pattern_index < pattern.size() && pattern[pattern_index] == kGlobstar) {
      star_pattern = ++pattern_index;
      star_path = path_index;
      continue;
    }
    if (pattern_index < pattern.size() &&
        MatchSegment(pattern[pattern_index], path[path_index])) {
      ++pattern_index;
      ++path_index;
      continue;
    }
    if (star_pattern == std::string_view::npos) {
      return false;
    }
    pattern_index = star_pattern;
    path_index = ++star_path;
  }
  while (pattern_index < pattern.size() &&
         pattern[pattern_index] == kGlobstar) {
    ++pattern_index;
  }
  return pattern_index == pattern.size();
}

std::vector<std::string_view> SplitPath(std::string_view path) {
  std::vector<std::string_view> segments;
  while (!path.empty()) {
    const std::size_t end = path.find('/');
    const std::string_view segment = path.substr(0, end);
    if (!segment.empty()) {
      segments.push_back(segment);
    }
    path = end == std::string_view::npos ? std::string_view{}
                                         : path.substr(end + 1);
  }
  return segments;
}

void ValidateSegment(std::string_view pattern, std::string_view segment) {
  const auto invalid = [&](std::string_view reason) {
    throw std::invalid_argument("invalid glob '" + std::string(pattern) +
                                "': " + std::string(reason));
  };
  if (segment.empty()) {
    invalid("empty path segment");
  }
  if (segment == "." || segment == "..") {
    invalid("'.' and '..' segments never match a repository path");
  }
  for (std::size_t index = 0; index < segment.size(); ++index) {
    if (segment[index] == '\\') {
      if (++index >= segment.size()) {
        invalid("trailing backslash");
      }
    } else if (segment[index] == '[') {
      std::size_t end = index + 1;
      if (end < segment.size() &&
          (segment[end] == '!' || segment[end] == '^')) {
        ++end;
      }
      if (end < segment.size() && segment[end] == ']') {
        ++end;
      }
      while (end < segment.size() && segment[end] != ']') {
        end += segment[end] == '\\' ? 2 : 1;
      }
      if (end >= segment.size()) {
        invalid("unterminated '['");
      }
      index = end;
    }
  }
}

}  // namespace

Glob Glob::Compile(std::string_view pattern) {
  if (pattern.empty()) {
    throw std::invalid_argument("glob patterns must not be empty");
  }
  if (pattern.front() == '/' || pattern.back() == '/') {
    throw std::invalid_argument(
        "invalid glob '" + std::string(pattern) +
        "': patterns are relative to the repository root and match files");
  }
  Glob glob;
  glob.pattern_ = pattern;
  std::string_view remaining = pattern;
  while (true) {
    const std::size_t end = remaining.find('/');
    const std::string_view segment = remaining.substr(0, end);
    ValidateSegment(pattern, segment);
    glob.segments_.emplace_back(segment);
    if (end == std::string_view::npos) {
      break;
    }
    remaining = remaining.substr(end + 1);
  }
  if (glob.segments_.back() == kGlobstar) {
    // A trailing "**" names the contents of a directory: one segment, then
    // any number more.
    glob.prunes_ = true;
    glob.prune_prefix_.assign(glob.segments_.begin(), glob.segments_.end() - 1);
    glob.segments_.back() = "*";
    glob.segments_.emplace_back(kGlobstar);
  }
  return glob;
}

bool Glob::Matches(std::string_view path) const {
  const std::vector<std::string_view> parts = SplitPath(path);
  return MatchSegments(segments_, parts);
}

bool Glob::PrunesDirectory(std::string_view directory) const {
  if (!prunes_) {
    return false;
  }
  const std::vector<std::string_view> parts = SplitPath(directory);
  const std::span<const std::string_view> all(parts);
  for (std::size_t length = 0; length <= parts.size(); ++length) {
    if (MatchSegments(prune_prefix_, all.first(length))) {
      return true;
    }
  }
  return false;
}

}  // namespace llmcc
