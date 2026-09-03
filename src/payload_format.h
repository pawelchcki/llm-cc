#ifndef LLM_CC_PAYLOAD_FORMAT_H_
#define LLM_CC_PAYLOAD_FORMAT_H_

#include <array>
#include <cstddef>
#include <cstdint>

namespace llmcc::payload {

inline constexpr std::array<char, 8> kFooterMagic = {'L', 'L', 'M', 'C',
                                                     'C', 'P', '0', '2'};
inline constexpr std::array<char, 8> kCudaMagic = {'L', 'L', 'M', 'C',
                                                   'U', 'D', '0', '2'};
inline constexpr std::array<char, 8> kRocmMagic = {'L', 'L', 'M', 'R',
                                                   'O', 'C', '0', '2'};
inline constexpr std::uint32_t kFooterVersion = 2;
inline constexpr std::uint32_t kPayloadCount = 2;
inline constexpr std::size_t kPayloadNameSize = 16;
inline constexpr std::size_t kSha256Size = 32;
inline constexpr std::size_t kFooterHeaderSize = 16;
inline constexpr std::size_t kFooterEntrySize = 64;
inline constexpr std::size_t kFooterSize =
    kFooterHeaderSize + (kPayloadCount * kFooterEntrySize);

}  // namespace llmcc::payload

#endif  // LLM_CC_PAYLOAD_FORMAT_H_
