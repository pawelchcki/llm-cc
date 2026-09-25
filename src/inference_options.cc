#include "src/inference_options.h"

#include <array>
#include <optional>
#include <string_view>

namespace llmcc {
namespace {

template <typename Enum, std::size_t Size>
std::optional<Enum> ParseName(std::string_view name,
                              const std::array<Enum, Size>& values,
                              std::string_view (*to_name)(Enum)) {
  for (const Enum value : values) {
    if (to_name(value) == name) {
      return value;
    }
  }
  return std::nullopt;
}

}  // namespace

std::string_view EntropyReductionName(EntropyReduction reduction) {
  switch (reduction) {
    case EntropyReduction::kAuto:
      return "auto";
    case EntropyReduction::kHost:
      return "host";
    case EntropyReduction::kDevice:
      return "device";
  }
  return "unknown";
}

std::string_view FlashAttentionName(FlashAttention setting) {
  switch (setting) {
    case FlashAttention::kAuto:
      return "auto";
    case FlashAttention::kOn:
      return "on";
    case FlashAttention::kOff:
      return "off";
  }
  return "unknown";
}

std::string_view KvCacheTypeName(KvCacheType type) {
  switch (type) {
    case KvCacheType::kF16:
      return "f16";
    case KvCacheType::kQ8_0:
      return "q8_0";
    case KvCacheType::kQ4_0:
      return "q4_0";
  }
  return "unknown";
}

std::optional<EntropyReduction> ParseEntropyReduction(std::string_view name) {
  return ParseName(name,
                   std::array{EntropyReduction::kAuto, EntropyReduction::kHost,
                              EntropyReduction::kDevice},
                   EntropyReductionName);
}

std::optional<FlashAttention> ParseFlashAttention(std::string_view name) {
  return ParseName(name,
                   std::array{FlashAttention::kAuto, FlashAttention::kOn,
                              FlashAttention::kOff},
                   FlashAttentionName);
}

std::optional<KvCacheType> ParseKvCacheType(std::string_view name) {
  return ParseName(
      name,
      std::array{KvCacheType::kF16, KvCacheType::kQ8_0, KvCacheType::kQ4_0},
      KvCacheTypeName);
}

}  // namespace llmcc
