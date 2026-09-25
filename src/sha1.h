#ifndef LLM_CC_SHA1_H_
#define LLM_CC_SHA1_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace llmcc {

// SHA-1 for Git object names only: a SHA-1 repository names blobs with it.
// It is never used where collision resistance matters.
class Sha1 {
 public:
  void Update(std::span<const char> bytes) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    std::size_t size = bytes.size();
    total_size_ += size;
    while (size > 0) {
      const std::size_t copied = std::min(size, buffer_.size() - buffer_size_);
      std::memcpy(buffer_.data() + buffer_size_, data, copied);
      buffer_size_ += copied;
      data += copied;
      size -= copied;
      if (buffer_size_ == buffer_.size()) {
        Compress();
        buffer_size_ = 0;
      }
    }
  }

  std::array<unsigned char, 20> Finish() {
    const std::uint64_t bit_size = total_size_ * 8;
    buffer_[buffer_size_++] = 0x80;
    if (buffer_size_ > 56) {
      std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_),
                buffer_.end(), 0);
      Compress();
      buffer_size_ = 0;
    }
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_),
              buffer_.begin() + 56, 0);
    for (std::size_t index = 0; index < 8; ++index) {
      buffer_[56 + index] =
          static_cast<unsigned char>(bit_size >> (56 - index * 8));
    }
    Compress();
    std::array<unsigned char, 20> result{};
    for (std::size_t word = 0; word < state_.size(); ++word) {
      for (std::size_t byte = 0; byte < 4; ++byte) {
        result[word * 4 + byte] = static_cast<unsigned char>(
            state_[word] >> (24 - static_cast<unsigned int>(byte) * 8));
      }
    }
    return result;
  }

 private:
  static std::uint32_t RotateLeft(std::uint32_t value, unsigned int shift) {
    return (value << shift) | (value >> (32 - shift));
  }

  void Compress() {
    std::array<std::uint32_t, 80> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      words[index] = static_cast<std::uint32_t>(buffer_[index * 4]) << 24 |
                     static_cast<std::uint32_t>(buffer_[index * 4 + 1]) << 16 |
                     static_cast<std::uint32_t>(buffer_[index * 4 + 2]) << 8 |
                     static_cast<std::uint32_t>(buffer_[index * 4 + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      words[index] = RotateLeft(words[index - 3] ^ words[index - 8] ^
                                    words[index - 14] ^ words[index - 16],
                                1);
    }
    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    for (std::size_t index = 0; index < words.size(); ++index) {
      std::uint32_t mixed = 0;
      std::uint32_t constant = 0;
      if (index < 20) {
        mixed = (b & c) | (~b & d);
        constant = 0x5a827999;
      } else if (index < 40) {
        mixed = b ^ c ^ d;
        constant = 0x6ed9eba1;
      } else if (index < 60) {
        mixed = (b & c) | (b & d) | (c & d);
        constant = 0x8f1bbcdc;
      } else {
        mixed = b ^ c ^ d;
        constant = 0xca62c1d6;
      }
      const std::uint32_t next =
          RotateLeft(a, 5) + mixed + e + constant + words[index];
      e = d;
      d = c;
      c = RotateLeft(b, 30);
      b = a;
      a = next;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
  }

  std::array<std::uint32_t, 5> state_ = {0x67452301, 0xefcdab89, 0x98badcfe,
                                         0x10325476, 0xc3d2e1f0};
  std::array<unsigned char, 64> buffer_{};
  std::uint64_t total_size_ = 0;
  std::size_t buffer_size_ = 0;
};

inline std::string Sha1Hex(std::string_view contents) {
  Sha1 hash;
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

#endif  // LLM_CC_SHA1_H_
