#ifndef LLM_CC_TOOLS_SHA256_H_
#define LLM_CC_TOOLS_SHA256_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace llmcc::tools {

class Sha256 {
 public:
  void Update(std::span<const char> bytes) {
    total_size_ += bytes.size();
    for (char value : bytes) {
      buffer_[buffer_size_++] = static_cast<unsigned char>(value);
      if (buffer_size_ == buffer_.size()) {
        Transform(buffer_);
        buffer_size_ = 0;
      }
    }
  }

  std::array<unsigned char, 32> Finish() {
    const std::uint64_t bit_size = total_size_ * 8;
    buffer_[buffer_size_++] = 0x80;
    if (buffer_size_ > 56) {
      while (buffer_size_ < buffer_.size()) {
        buffer_[buffer_size_++] = 0;
      }
      Transform(buffer_);
      buffer_size_ = 0;
    }
    while (buffer_size_ < 56) {
      buffer_[buffer_size_++] = 0;
    }
    for (std::size_t index = 0; index < 8; ++index) {
      buffer_[56 + index] =
          static_cast<unsigned char>(bit_size >> (56 - index * 8));
    }
    Transform(buffer_);

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
  static std::uint32_t RotateRight(std::uint32_t value, unsigned int shift) {
    return (value >> shift) | (value << (32 - shift));
  }

  void Transform(const std::array<unsigned char, 64>& block) {
    static constexpr std::array<std::uint32_t, 64> kRoundConstants = {
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
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      words[index] = static_cast<std::uint32_t>(block[index * 4]) << 24 |
                     static_cast<std::uint32_t>(block[index * 4 + 1]) << 16 |
                     static_cast<std::uint32_t>(block[index * 4 + 2]) << 8 |
                     static_cast<std::uint32_t>(block[index * 4 + 3]);
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

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];
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
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_ = {
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
  };
  std::array<unsigned char, 64> buffer_{};
  std::uint64_t total_size_ = 0;
  std::size_t buffer_size_ = 0;
};

}  // namespace llmcc::tools

#endif  // LLM_CC_TOOLS_SHA256_H_
