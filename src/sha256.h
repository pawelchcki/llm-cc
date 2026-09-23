#ifndef LLM_CC_SHA256_H_
#define LLM_CC_SHA256_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

// GCC and Clang can compile SHA extension code without enabling it for the
// whole translation unit. The instructions are used only after CPUID confirms
// them, so the build targets and hosts without them are unaffected.
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#include <immintrin.h>
#define LLM_CC_SHA256_X86_EXTENSIONS 1
#endif

namespace llmcc {
namespace sha256_detail {

inline constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

using State = std::array<std::uint32_t, 8>;

inline std::uint32_t RotateRight(std::uint32_t value, unsigned int shift) {
  return (value >> shift) | (value << (32 - shift));
}

inline void CompressPortable(State& state, const unsigned char* blocks,
                             std::size_t count) {
  for (; count > 0; --count, blocks += 64) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      words[index] = static_cast<std::uint32_t>(blocks[index * 4]) << 24 |
                     static_cast<std::uint32_t>(blocks[index * 4 + 1]) << 16 |
                     static_cast<std::uint32_t>(blocks[index * 4 + 2]) << 8 |
                     static_cast<std::uint32_t>(blocks[index * 4 + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const std::uint32_t sigma0 = RotateRight(words[index - 15], 7) ^
                                   RotateRight(words[index - 15], 18) ^
                                   (words[index - 15] >> 3);
      const std::uint32_t sigma1 = RotateRight(words[index - 2], 17) ^
                                   RotateRight(words[index - 2], 19) ^
                                   (words[index - 2] >> 10);
      words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const std::uint32_t choice = (e & f) ^ (~e & g);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t sum0 =
          RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
      const std::uint32_t sum1 =
          RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
      const std::uint32_t first =
          h + sum1 + choice + kRoundConstants[index] + words[index];
      const std::uint32_t second = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + first;
      d = c;
      c = b;
      b = a;
      a = first + second;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }
}

#if defined(LLM_CC_SHA256_X86_EXTENSIONS)
inline bool HasShaExtensions() {
  unsigned int eax = 0;
  unsigned int ebx = 0;
  unsigned int ecx = 0;
  unsigned int edx = 0;
  if (__get_cpuid_max(0, nullptr) < 7 ||
      __get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0) {
    return false;
  }
  const bool ssse3 = (ecx & (1U << 9)) != 0;
  const bool sse41 = (ecx & (1U << 19)) != 0;
  __cpuid_count(7, 0, eax, ebx, ecx, edx);
  return ssse3 && sse41 && (ebx & (1U << 29)) != 0;
}

// The Intel SHA extensions keep the state as ABEF/CDGH lane pairs and consume
// two rounds per instruction; the message schedule is extended four words at
// a time from the previous sixteen.
__attribute__((target("sha,sse4.1,ssse3"))) inline void CompressShaExtensions(
    State& state, const unsigned char* blocks, std::size_t count) {
  const __m128i byte_swap =
      _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
  __m128i dcba = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[0]));
  __m128i hgfe = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[4]));
  const __m128i cdab = _mm_shuffle_epi32(dcba, 0xb1);
  const __m128i efgh = _mm_shuffle_epi32(hgfe, 0x1b);
  __m128i abef = _mm_alignr_epi8(cdab, efgh, 8);
  __m128i cdgh = _mm_blend_epi16(efgh, cdab, 0xf0);

  for (; count > 0; --count, blocks += 64) {
    const __m128i abef_before = abef;
    const __m128i cdgh_before = cdgh;
    __m128i schedule[4];
    for (std::size_t index = 0; index < 4; ++index) {
      schedule[index] = _mm_shuffle_epi8(
          _mm_loadu_si128(
              reinterpret_cast<const __m128i*>(blocks + index * 16)),
          byte_swap);
    }
    for (std::size_t group = 0; group < 16; ++group) {
      const __m128i message = _mm_add_epi32(
          schedule[group % 4], _mm_loadu_si128(reinterpret_cast<const __m128i*>(
                                   &kRoundConstants[group * 4])));
      cdgh = _mm_sha256rnds2_epu32(cdgh, abef, message);
      abef =
          _mm_sha256rnds2_epu32(abef, cdgh, _mm_shuffle_epi32(message, 0x0e));
      if (group < 12) {
        // Words 4(group+4)..4(group+4)+3 from the four preceding groups.
        __m128i next = _mm_sha256msg1_epu32(schedule[group % 4],
                                            schedule[(group + 1) % 4]);
        next =
            _mm_add_epi32(next, _mm_alignr_epi8(schedule[(group + 3) % 4],
                                                schedule[(group + 2) % 4], 4));
        schedule[group % 4] =
            _mm_sha256msg2_epu32(next, schedule[(group + 3) % 4]);
      }
    }
    abef = _mm_add_epi32(abef, abef_before);
    cdgh = _mm_add_epi32(cdgh, cdgh_before);
  }

  const __m128i feba = _mm_shuffle_epi32(abef, 0x1b);
  const __m128i dchg = _mm_shuffle_epi32(cdgh, 0xb1);
  dcba = _mm_blend_epi16(feba, dchg, 0xf0);
  hgfe = _mm_alignr_epi8(dchg, feba, 8);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[0]), dcba);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[4]), hgfe);
}
#endif

inline bool AcceleratedCompressionAvailable() {
#if defined(LLM_CC_SHA256_X86_EXTENSIONS)
  static const bool available = HasShaExtensions();
  return available;
#else
  return false;
#endif
}

inline void Compress(State& state, const unsigned char* blocks,
                     std::size_t count) {
#if defined(LLM_CC_SHA256_X86_EXTENSIONS)
  if (AcceleratedCompressionAvailable()) {
    CompressShaExtensions(state, blocks, count);
    return;
  }
#endif
  CompressPortable(state, blocks, count);
}

}  // namespace sha256_detail

class Sha256 {
 public:
  void Update(std::span<const char> bytes) {
    if (bytes.empty()) {
      return;
    }
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    std::size_t size = bytes.size();
    total_size_ += size;
    if (buffer_size_ > 0) {
      const std::size_t copied = std::min(size, buffer_.size() - buffer_size_);
      std::memcpy(buffer_.data() + buffer_size_, data, copied);
      buffer_size_ += copied;
      data += copied;
      size -= copied;
      if (buffer_size_ < buffer_.size()) {
        return;
      }
      sha256_detail::Compress(state_, buffer_.data(), 1);
      buffer_size_ = 0;
    }
    // Whole blocks are compressed straight from the caller's memory.
    const std::size_t blocks = size / buffer_.size();
    if (blocks > 0) {
      sha256_detail::Compress(state_, data, blocks);
      data += blocks * buffer_.size();
      size -= blocks * buffer_.size();
    }
    if (size > 0) {
      std::memcpy(buffer_.data(), data, size);
      buffer_size_ = size;
    }
  }

  std::array<unsigned char, 32> Finish() {
    const std::uint64_t bit_size = total_size_ * 8;
    buffer_[buffer_size_++] = 0x80;
    if (buffer_size_ > 56) {
      while (buffer_size_ < buffer_.size()) {
        buffer_[buffer_size_++] = 0;
      }
      sha256_detail::Compress(state_, buffer_.data(), 1);
      buffer_size_ = 0;
    }
    while (buffer_size_ < 56) {
      buffer_[buffer_size_++] = 0;
    }
    for (std::size_t index = 0; index < 8; ++index) {
      buffer_[56 + index] =
          static_cast<unsigned char>(bit_size >> (56 - index * 8));
    }
    sha256_detail::Compress(state_, buffer_.data(), 1);

    std::array<unsigned char, 32> result{};
    for (std::size_t word = 0; word < state_.size(); ++word) {
      for (std::size_t byte = 0; byte < 4; ++byte) {
        result[word * 4 + byte] = static_cast<unsigned char>(
            state_[word] >> (24 - static_cast<unsigned int>(byte) * 8));
      }
    }
    return result;
  }

 private:
  sha256_detail::State state_ = {
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
  };
  std::array<unsigned char, 64> buffer_{};
  std::uint64_t total_size_ = 0;
  std::size_t buffer_size_ = 0;
};

inline std::string Sha256Hex(std::string_view contents) {
  Sha256 hash;
  hash.Update(std::span<const char>(contents.data(), contents.size()));
  const auto digest = hash.Finish();
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2);
  for (const unsigned char byte : digest) {
    result.push_back(kDigits[byte >> 4]);
    result.push_back(kDigits[byte & 0x0f]);
  }
  return result;
}

}  // namespace llmcc

#endif  // LLM_CC_SHA256_H_
