#include "src/scoring.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace rethink {

TokenScore ScoreToken(std::span<const float> logits, std::size_t token_id) {
  if (logits.empty()) {
    throw std::invalid_argument("cannot score an empty logit row");
  }
  if (token_id >= logits.size()) {
    throw std::out_of_range("token id is outside the vocabulary");
  }

  double maximum = -std::numeric_limits<double>::infinity();
  for (float logit : logits) {
    maximum = std::max(maximum, static_cast<double>(logit));
  }

  double denominator = 0.0;
  for (float logit : logits) {
    denominator += std::exp(static_cast<double>(logit) - maximum);
  }

  const double log_probability =
      static_cast<double>(logits[token_id]) - maximum - std::log(denominator);
  return {.probability = std::exp(log_probability),
          .log_probability = log_probability};
}

std::string BytesToHex(std::string_view bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (unsigned char byte : bytes) {
    result.push_back(kHex[byte >> 4]);
    result.push_back(kHex[byte & 0x0f]);
  }
  return result;
}

namespace {

bool IsContinuation(unsigned char byte) { return (byte & 0xc0) == 0x80; }

std::size_t ValidUtf8Length(std::string_view value, std::size_t offset) {
  const auto first = static_cast<unsigned char>(value[offset]);
  if (first < 0x80) {
    return 1;
  }

  std::size_t length = 0;
  unsigned int minimum = 0;
  unsigned int codepoint = 0;
  if ((first & 0xe0) == 0xc0) {
    length = 2;
    minimum = 0x80;
    codepoint = first & 0x1f;
  } else if ((first & 0xf0) == 0xe0) {
    length = 3;
    minimum = 0x800;
    codepoint = first & 0x0f;
  } else if ((first & 0xf8) == 0xf0) {
    length = 4;
    minimum = 0x10000;
    codepoint = first & 0x07;
  } else {
    return 0;
  }
  if (offset + length > value.size()) {
    return 0;
  }
  for (std::size_t i = 1; i < length; ++i) {
    const auto byte = static_cast<unsigned char>(value[offset + i]);
    if (!IsContinuation(byte)) {
      return 0;
    }
    codepoint = (codepoint << 6) | (byte & 0x3f);
  }
  if (codepoint < minimum || codepoint > 0x10ffff ||
      (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
    return 0;
  }
  return length;
}

}  // namespace

std::string JsonEscapeBytes(std::string_view bytes) {
  std::ostringstream output;
  for (std::size_t i = 0; i < bytes.size();) {
    const auto byte = static_cast<unsigned char>(bytes[i]);
    switch (byte) {
      case '"':
        output << "\\\"";
        ++i;
        continue;
      case '\\':
        output << "\\\\";
        ++i;
        continue;
      case '\b':
        output << "\\b";
        ++i;
        continue;
      case '\f':
        output << "\\f";
        ++i;
        continue;
      case '\n':
        output << "\\n";
        ++i;
        continue;
      case '\r':
        output << "\\r";
        ++i;
        continue;
      case '\t':
        output << "\\t";
        ++i;
        continue;
      default:
        break;
    }
    if (byte < 0x20) {
      output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
             << static_cast<unsigned int>(byte) << std::dec;
      ++i;
      continue;
    }
    const std::size_t utf8_length = ValidUtf8Length(bytes, i);
    if (utf8_length == 0) {
      output << "\\ufffd";
      ++i;
      continue;
    }
    output.write(bytes.data() + i, static_cast<std::streamsize>(utf8_length));
    i += utf8_length;
  }
  return output.str();
}

}  // namespace rethink
