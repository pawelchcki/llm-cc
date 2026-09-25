#ifndef LLM_CC_INFERENCE_OPTIONS_H_
#define LLM_CC_INFERENCE_OPTIONS_H_

#include <cstdint>
#include <optional>
#include <string_view>

namespace llmcc {

inline constexpr std::uint32_t kDefaultContextSize = 128U * 1024U;
inline constexpr std::uint32_t kDefaultBatchSize = 64;

enum class EntropyReduction : std::uint8_t { kAuto, kHost, kDevice };
std::string_view EntropyReductionName(EntropyReduction reduction);

enum class FlashAttention : std::uint8_t { kAuto, kOn, kOff };
enum class KvCacheType : std::uint8_t { kF16, kQ8_0, kQ4_0 };
std::string_view FlashAttentionName(FlashAttention setting);
std::string_view KvCacheTypeName(KvCacheType type);

// The inverses of the names above; nullopt for any other spelling.
std::optional<EntropyReduction> ParseEntropyReduction(std::string_view name);
std::optional<FlashAttention> ParseFlashAttention(std::string_view name);
std::optional<KvCacheType> ParseKvCacheType(std::string_view name);

}  // namespace llmcc

#endif  // LLM_CC_INFERENCE_OPTIONS_H_
