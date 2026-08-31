#include "src/scoring.h"

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace llmcc {

namespace {

constexpr std::uint64_t kContextOverheadBytes = 512ULL * 1024 * 1024;

std::uint64_t SaturatingAdd(std::uint64_t left, std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

std::uint64_t EstimatedWeightBytes(std::uint64_t model_bytes) {
  const std::uint64_t safety_margin =
      (model_bytes / 10) + (model_bytes % 10 != 0 ? 1 : 0);
  return SaturatingAdd(model_bytes, safety_margin);
}

std::string FormatBytes(std::uint64_t bytes) {
  constexpr long double kGiB = 1024.0L * 1024.0L * 1024.0L;
  std::ostringstream output;
  output << std::fixed << std::setprecision(2)
         << static_cast<long double>(bytes) / kGiB << " GiB (" << bytes
         << " bytes)";
  return output.str();
}

}  // namespace

MemoryCheckResult CheckMemory(std::uint64_t model_bytes,
                              std::uint64_t host_available_bytes,
                              std::optional<std::uint64_t> gpu_available_bytes,
                              bool use_gpu, bool override_check) {
  if (override_check) {
    return {.ok = true, .error = {}};
  }

  const std::uint64_t weight_bytes = EstimatedWeightBytes(model_bytes);
  if (!use_gpu) {
    const std::uint64_t host_required =
        SaturatingAdd(weight_bytes, kContextOverheadBytes);
    if (host_required <= host_available_bytes) {
      return {.ok = true, .error = {}};
    }
    return {
        .ok = false,
        .error = "insufficient memory: host required " +
                 FormatBytes(host_required) + ", available " +
                 FormatBytes(host_available_bytes) + "; GPU not used",
    };
  }

  if (!gpu_available_bytes.has_value()) {
    return {
        .ok = false,
        .error = "GPU acceleration selected, but the build has no GPU backend"};
  }

  // Heuristic: when any layers are offloaded, all model weights count against
  // VRAM and the fixed context overhead counts against host RAM. Actual partial
  // offload allocations vary by architecture and requested layer count.
  const std::uint64_t host_required = kContextOverheadBytes;
  const std::uint64_t gpu_required = weight_bytes;
  if (host_required <= host_available_bytes &&
      gpu_required <= *gpu_available_bytes) {
    return {.ok = true, .error = {}};
  }
  return {
      .ok = false,
      .error = "insufficient memory: host required " +
               FormatBytes(host_required) + ", available " +
               FormatBytes(host_available_bytes) + "; GPU required " +
               FormatBytes(gpu_required) + ", available " +
               FormatBytes(*gpu_available_bytes),
  };
}

TokenScore ScoreToken(std::span<const float> logits, std::size_t token_id,
                      bool compute_entropy) {
  if (logits.empty()) {
    throw std::invalid_argument("cannot score an empty logit row");
  }
  if (token_id >= logits.size()) {
    throw std::out_of_range("token id is outside the vocabulary");
  }

  double maximum = -std::numeric_limits<double>::infinity();
  double denominator = 0.0;
  double weighted_shifted_logits = 0.0;
  for (float raw_logit : logits) {
    const double logit = static_cast<double>(raw_logit);
    if (logit == -std::numeric_limits<double>::infinity()) {
      continue;
    }
    if (!std::isfinite(logit)) {
      throw std::invalid_argument("logits must not contain NaN or +infinity");
    }
    if (denominator == 0.0) {
      maximum = logit;
      denominator = 1.0;
      continue;
    }
    if (logit > maximum) {
      const double maximum_shift = maximum - logit;
      const double scale = std::exp(maximum_shift);
      if (compute_entropy) {
        weighted_shifted_logits =
            scale * (weighted_shifted_logits + (maximum_shift * denominator));
      }
      denominator = (scale * denominator) + 1.0;
      maximum = logit;
    } else {
      const double shifted_logit = logit - maximum;
      const double weight = std::exp(shifted_logit);
      denominator += weight;
      if (compute_entropy) {
        weighted_shifted_logits += shifted_logit * weight;
      }
    }
  }
  if (denominator == 0.0) {
    throw std::invalid_argument("logit row has no finite value");
  }

  const double log_probability =
      static_cast<double>(logits[token_id]) - maximum - std::log(denominator);
  const std::optional<double> entropy =
      compute_entropy
          ? std::optional<double>(std::log(denominator) -
                                  (weighted_shifted_logits / denominator))
          : std::nullopt;
  return {.probability = std::exp(log_probability),
          .log_probability = log_probability,
          .entropy = entropy};
}

std::string BytesToHex(std::string_view bytes) {
  static constexpr std::string_view kHex = "0123456789abcdef";
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

}  // namespace llmcc
