#include "src/compare/json_util.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "src/cache_io.h"
#include "src/sha256.h"

namespace llmcc::compare {
namespace {

constexpr std::string_view kHexDigits = "0123456789abcdef";

void AppendUnicodeEscape(std::string& output, unsigned int unit) {
  output += "\\u";
  for (int shift = 12; shift >= 0; shift -= 4) {
    output.push_back(
        kHexDigits[(unit >> static_cast<unsigned int>(shift)) & 0xFU]);
  }
}

// Decodes one UTF-8 sequence at `index`, advancing it. Throws for malformed
// or overlong input and for surrogate code points.
char32_t DecodeUtf8(std::string_view text, std::size_t& index) {
  const auto lead = static_cast<unsigned char>(text[index]);
  if (lead < 0x80) {
    ++index;
    return lead;
  }
  std::size_t length = 0;
  char32_t value = 0;
  char32_t minimum = 0;
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
    throw std::invalid_argument("string is not valid UTF-8");
  }
  if (index + length > text.size()) {
    throw std::invalid_argument("string is not valid UTF-8");
  }
  for (std::size_t offset = 1; offset < length; ++offset) {
    const auto next = static_cast<unsigned char>(text[index + offset]);
    if ((next & 0xC0U) != 0x80U) {
      throw std::invalid_argument("string is not valid UTF-8");
    }
    value = (value << 6U) | (next & 0x3FU);
  }
  if (value < minimum || value > 0x10FFFF ||
      (value >= 0xD800 && value <= 0xDFFF)) {
    throw std::invalid_argument("string is not valid UTF-8");
  }
  index += length;
  return value;
}

void AppendString(std::string& output, std::string_view text) {
  output.push_back('"');
  std::size_t index = 0;
  while (index < text.size()) {
    const char32_t code = DecodeUtf8(text, index);
    switch (code) {
      case U'"':
        output += "\\\"";
        break;
      case U'\\':
        output += "\\\\";
        break;
      case U'\n':
        output += "\\n";
        break;
      case U'\r':
        output += "\\r";
        break;
      case U'\t':
        output += "\\t";
        break;
      case U'\b':
        output += "\\b";
        break;
      case U'\f':
        output += "\\f";
        break;
      default:
        if (code >= 0x20 && code < 0x7F) {
          output.push_back(static_cast<char>(code));
        } else if (code <= 0xFFFF) {
          AppendUnicodeEscape(output, static_cast<unsigned int>(code));
        } else {
          const char32_t offset = code - 0x10000;
          AppendUnicodeEscape(
              output, static_cast<unsigned int>(0xD800 + (offset >> 10U)));
          AppendUnicodeEscape(
              output, static_cast<unsigned int>(0xDC00 + (offset & 0x3FFU)));
        }
    }
  }
  output.push_back('"');
}

// Python's float repr: the shortest round-trip digits, positional for
// exponents from -4 through 15 and scientific otherwise.
std::string PythonFloat(double value) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument("non-finite numbers cannot be encoded");
  }
  std::array<char, 64> buffer{};
  const auto converted =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                    std::chars_format::scientific);
  std::string_view text(
      buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data()));
  std::string result;
  if (text.front() == '-') {
    result.push_back('-');
    text.remove_prefix(1);
  }
  const std::size_t exponent_at = text.find('e');
  std::string digits;
  for (const char character : text.substr(0, exponent_at)) {
    if (character != '.') {
      digits.push_back(character);
    }
  }
  int exponent = 0;
  const std::string_view exponent_text = text.substr(exponent_at + 1);
  std::from_chars(exponent_text.data() + (exponent_text.front() == '+' ? 1 : 0),
                  exponent_text.data() + exponent_text.size(), exponent);
  if (exponent >= -4 && exponent < 16) {
    if (exponent < 0) {
      result += "0." +
                std::string(static_cast<std::size_t>(-exponent - 1), '0') +
                digits;
    } else {
      const auto integer_digits = static_cast<std::size_t>(exponent) + 1;
      if (digits.size() <= integer_digits) {
        result +=
            digits + std::string(integer_digits - digits.size(), '0') + ".0";
      } else {
        result += digits.substr(0, integer_digits) + "." +
                  digits.substr(integer_digits);
      }
    }
    return result;
  }
  result.push_back(digits.front());
  if (digits.size() > 1) {
    result += "." + digits.substr(1);
  }
  result += exponent < 0 ? "e-" : "e+";
  const std::string magnitude = std::to_string(std::abs(exponent));
  if (magnitude.size() < 2) {
    result.push_back('0');
  }
  return result + magnitude;
}

void AppendCanonical(std::string& output, const nlohmann::json& value) {
  switch (value.type()) {
    case nlohmann::json::value_t::null:
      output += "null";
      return;
    case nlohmann::json::value_t::boolean:
      output += value.get<bool>() ? "true" : "false";
      return;
    case nlohmann::json::value_t::number_integer:
      output += std::to_string(value.get<std::int64_t>());
      return;
    case nlohmann::json::value_t::number_unsigned:
      output += std::to_string(value.get<std::uint64_t>());
      return;
    case nlohmann::json::value_t::number_float:
      output += PythonFloat(value.get<double>());
      return;
    case nlohmann::json::value_t::string:
      AppendString(output, value.get_ref<const std::string&>());
      return;
    case nlohmann::json::value_t::array: {
      output.push_back('[');
      bool first = true;
      for (const nlohmann::json& item : value) {
        if (!first) {
          output.push_back(',');
        }
        first = false;
        AppendCanonical(output, item);
      }
      output.push_back(']');
      return;
    }
    case nlohmann::json::value_t::object: {
      output.push_back('{');
      bool first = true;
      for (const auto& [key, item] : value.items()) {
        if (!first) {
          output.push_back(',');
        }
        first = false;
        AppendString(output, key);
        output.push_back(':');
        AppendCanonical(output, item);
      }
      output.push_back('}');
      return;
    }
    case nlohmann::json::value_t::binary:
    case nlohmann::json::value_t::discarded:
      break;
  }
  throw std::invalid_argument("value has no JSON encoding");
}

}  // namespace

std::string CanonicalJson(const nlohmann::json& value) {
  std::string output;
  AppendCanonical(output, value);
  return output;
}

std::string CanonicalDigest(const nlohmann::json& value) {
  return Sha256Hex(CanonicalJson(value));
}

std::string ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open " + path.string());
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  if (input.bad()) {
    throw std::runtime_error("cannot read " + path.string());
  }
  return contents.str();
}

nlohmann::json ReadJsonFile(const std::filesystem::path& path) {
  const std::string text = ReadFileBytes(path);
  try {
    return nlohmann::json::parse(text);
  } catch (const nlohmann::json::parse_error& error) {
    throw std::runtime_error(path.string() +
                             " is not valid JSON: " + error.what());
  }
}

void WriteFileAtomic(const std::filesystem::path& path,
                     std::string_view contents) {
  // Replaces an existing file on every platform, and never a symlink.
  cache_io::AtomicWriteFile(path, contents, 0666);
}

void WriteJsonFile(const std::filesystem::path& path,
                   const nlohmann::json& value) {
  WriteFileAtomic(path, CanonicalJson(value) + "\n");
}

bool IsInteger(const nlohmann::json& value) {
  return value.is_number_integer();
}

bool IsNumber(const nlohmann::json& value) { return value.is_number(); }

bool Truthy(const nlohmann::json& value) {
  switch (value.type()) {
    case nlohmann::json::value_t::null:
    case nlohmann::json::value_t::discarded:
      return false;
    case nlohmann::json::value_t::boolean:
      return value.get<bool>();
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned:
    case nlohmann::json::value_t::number_float:
      return value.get<double>() != 0.0;
    case nlohmann::json::value_t::string:
      return !value.get_ref<const std::string&>().empty();
    case nlohmann::json::value_t::array:
    case nlohmann::json::value_t::object:
    case nlohmann::json::value_t::binary:
      return !value.empty();
  }
  return false;
}

}  // namespace llmcc::compare
