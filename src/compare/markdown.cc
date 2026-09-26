#include "src/compare/markdown.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace llmcc::compare::markdown {
namespace {

struct CodePointRange {
  char32_t first;
  char32_t last;
};

constexpr CodePointRange kFormatCharacters[] = {
// NOLINT(modernize-avoid-c-arrays)
#include "src/compare/unicode_cf.inc"
};

constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr std::string_view kEllipsis = "\xE2\x80\xA6";
constexpr std::string_view kTruncationNotice =
    "\n_Full details are available in artifacts._\n";

bool IsFormatCharacter(char32_t code) {
  const auto* range = std::ranges::upper_bound(
      kFormatCharacters, code, {},
      [](const CodePointRange& entry) { return entry.first; });
  return range != std::begin(kFormatCharacters) && code <= (range - 1)->last;
}

bool Hidden(char32_t code) {
  return code < 0x20 || (code >= 0x7F && code <= 0x9F) || code == 0x2028 ||
         code == 0x2029 || IsFormatCharacter(code);
}

void AppendEscape(std::string& output, char32_t code) {
  std::string digits;
  auto value = static_cast<std::uint32_t>(code);
  do {
    digits.insert(digits.begin(), kHexDigits[value & 0xFU]);
    value >>= 4U;
  } while (value != 0);
  output += "\\u" +
            std::string(digits.size() < 4 ? 4 - digits.size() : 0, '0') +
            digits;
}

void AppendUtf8(std::string& output, char32_t code) {
  if (code < 0x80) {
    output.push_back(static_cast<char>(code));
  } else if (code < 0x800) {
    output.push_back(static_cast<char>(0xC0 | (code >> 6U)));
    output.push_back(static_cast<char>(0x80 | (code & 0x3FU)));
  } else if (code < 0x10000) {
    output.push_back(static_cast<char>(0xE0 | (code >> 12U)));
    output.push_back(static_cast<char>(0x80 | ((code >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80 | (code & 0x3FU)));
  } else {
    output.push_back(static_cast<char>(0xF0 | (code >> 18U)));
    output.push_back(static_cast<char>(0x80 | ((code >> 12U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80 | ((code >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80 | (code & 0x3FU)));
  }
}

// Decodes one well-formed UTF-8 sequence; nullopt leaves `index` alone.
std::optional<char32_t> DecodeUtf8(std::string_view text, std::size_t index,
                                   std::size_t& length) {
  const auto lead = static_cast<unsigned char>(text[index]);
  char32_t value = 0;
  char32_t minimum = 0;
  if (lead < 0x80) {
    length = 1;
    return lead;
  }
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
    value = lead & 0x1FU;
    minimum = 0x80;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    value = lead & 0x0FU;
    minimum = 0x800;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    value = lead & 0x07U;
    minimum = 0x10000;
  } else {
    return std::nullopt;
  }
  if (index + length > text.size()) {
    return std::nullopt;
  }
  for (std::size_t offset = 1; offset < length; ++offset) {
    const auto next = static_cast<unsigned char>(text[index + offset]);
    if ((next & 0xC0U) != 0x80U) {
      return std::nullopt;
    }
    value = (value << 6U) | (next & 0x3FU);
  }
  if (value < minimum || value > 0x10FFFF ||
      (value >= 0xD800 && value <= 0xDFFF)) {
    return std::nullopt;
  }
  return value;
}

std::vector<char32_t> CodePoints(std::string_view text) {
  std::vector<char32_t> points;
  std::size_t index = 0;
  while (index < text.size()) {
    std::size_t length = 1;
    const auto code = DecodeUtf8(text, index, length);
    points.push_back(code.value_or(static_cast<unsigned char>(text[index])));
    index += code.has_value() ? length : 1;
  }
  return points;
}

std::string Encode(std::span<const char32_t> points) {
  std::string output;
  for (const char32_t code : points) {
    AppendUtf8(output, code);
  }
  return output;
}

std::string Format(const char* format, double value) {
  std::array<char, 128> buffer{};
  const int size =
      std::snprintf(buffer.data(), buffer.size(), format, value);  // NOLINT
  return {buffer.data(), static_cast<std::size_t>(std::max(size, 0))};
}

std::string RenderLines(const std::vector<const Section*>& kept) {
  std::string text;
  bool first = true;
  for (const Section* section : kept) {
    for (const std::string& line : section->lines) {
      if (!first) {
        text.push_back('\n');
      }
      first = false;
      text += line;
    }
  }
  while (!text.empty() && text.back() == '\n') {
    text.pop_back();
  }
  return text + "\n";
}

}  // namespace

std::string NeutralizeControls(std::string_view text) {
  std::string output;
  output.reserve(text.size());
  std::size_t index = 0;
  while (index < text.size()) {
    std::size_t length = 1;
    const auto code = DecodeUtf8(text, index, length);
    if (!code.has_value()) {
      // Python decodes such bytes to lone surrogates, shown as \udcXX.
      AppendEscape(output, 0xDC00U + static_cast<unsigned char>(text[index]));
      ++index;
      continue;
    }
    if (Hidden(*code)) {
      AppendEscape(output, *code);
    } else {
      output.append(text.substr(index, length));
    }
    index += length;
  }
  return output;
}

std::string Code(std::string_view text, bool table) {
  std::string value = NeutralizeControls(text);
  const std::vector<char32_t> points = CodePoints(value);
  if (points.size() > kPathDisplayLimit) {
    const std::size_t head = (kPathDisplayLimit - 1) / 2;
    const std::size_t tail = kPathDisplayLimit - 1 - head;
    const std::span<const char32_t> all(points);
    value = Encode(all.first(head)) + std::string(kEllipsis) +
            Encode(all.last(tail));
  }
  if (table) {
    // GFM splits table cells on every unescaped pipe, code spans included.
    // Double any backslash run already in front of a pipe: "a\\|b" would
    // otherwise escape the backslash and leave the pipe splitting the cell.
    // Backslashes elsewhere are literal and stay as they are.
    std::string escaped;
    std::size_t backslashes = 0;
    for (const char character : value) {
      if (character == '\\') {
        ++backslashes;
      } else if (character == '|') {
        escaped.append(backslashes * 2, '\\');
        escaped += "\\|";
        backslashes = 0;
      } else {
        escaped.append(backslashes, '\\');
        backslashes = 0;
        escaped.push_back(character);
      }
    }
    escaped.append(backslashes, '\\');
    value = std::move(escaped);
  }
  std::size_t longest = 0;
  std::size_t run = 0;
  for (const char character : value) {
    run = character == '`' ? run + 1 : 0;
    longest = std::max(longest, run);
  }
  const std::string fence(longest + 1, '`');
  const std::string pad =
      value.empty() || value.front() == '`' || value.back() == '`' ? " " : "";
  return fence + pad + value + pad + fence;
}

std::string Assemble(const std::vector<Section>& sections, std::size_t limit) {
  std::vector<const Section*> kept;
  kept.reserve(sections.size());
  for (const Section& section : sections) {
    kept.push_back(&section);
  }
  std::string payload = RenderLines(kept);
  const auto priorities = [&] {
    std::set<int> values;
    for (const Section* section : kept) {
      values.insert(section->priority);
    }
    return values;
  };
  while (payload.size() > limit && priorities().size() > 1) {
    const int lowest = *priorities().rbegin();
    std::erase_if(kept, [lowest](const Section* section) {
      return section->priority == lowest;
    });
    payload = RenderLines(kept);
  }
  if (payload.size() > limit) {
    const std::string head =
        payload.substr(0, limit - std::min(limit, kTruncationNotice.size()));
    const std::size_t cut = head.rfind('\n');
    payload = (cut == std::string::npos ? head : head.substr(0, cut + 1)) +
              std::string(kTruncationNotice);
  }
  return payload;
}

std::string Fmt(const nlohmann::json& value) {
  return value.is_null() ? "unavailable" : Format("%.6g", value.get<double>());
}

std::string Raw(const nlohmann::json& value, bool sign) {
  return value.is_null() ? "unavailable"
                         : Format(sign ? "%+.1f" : "%.1f", value.get<double>());
}

std::string Percent(const nlohmann::json& value) {
  return value.is_null() ? "unavailable"
                         : Format("%+.3g", value.get<double>()) + "%";
}

}  // namespace llmcc::compare::markdown
