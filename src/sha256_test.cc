#include "src/sha256.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "src/test_util.h"

namespace {

std::string Hex(const std::array<unsigned char, 32>& digest) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string result;
  for (const unsigned char byte : digest) {
    result.push_back(kDigits[byte >> 4]);
    result.push_back(kDigits[byte & 0x0f]);
  }
  return result;
}

std::string HashInChunks(const std::string& input, std::size_t chunk) {
  llmcc::Sha256 hash;
  for (std::size_t offset = 0; offset < input.size(); offset += chunk) {
    const std::size_t size = std::min(chunk, input.size() - offset);
    hash.Update(std::span<const char>(input.data() + offset, size));
  }
  return Hex(hash.Finish());
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  struct Vector {
    std::string input;
    std::string digest;
  };
  // FIPS 180-4 examples plus a message whose padding needs a second block.
  const std::vector<Vector> vectors = {
      {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {"abc",
       "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
      {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmno"
       "pjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
       "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
      {std::string(1000000, 'a'),
       "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
  };
  for (const Vector& vector : vectors) {
    llmcc::test::ExpectEq(llmcc::Sha256Hex(vector.input), vector.digest,
                          "one-shot digest matches FIPS 180-4");
    for (const std::size_t chunk : {1U, 3U, 63U, 64U, 65U, 1000U}) {
      llmcc::test::ExpectEq(HashInChunks(vector.input, chunk), vector.digest,
                            "chunked digest matches the one-shot digest");
    }
  }

  // Cover every buffer alignment around block boundaries with irregular bytes.
  std::string data(4096 + 129, '\0');
  std::uint32_t seed = 0x12345678;
  for (char& byte : data) {
    seed = seed * 1664525U + 1013904223U;
    byte = static_cast<char>(seed >> 24);
  }
  const bool accelerated =
      llmcc::sha256_detail::AcceleratedCompressionAvailable();
  std::cout << "accelerated compression: " << (accelerated ? "yes" : "no")
            << '\n';
  for (std::size_t blocks = 0; blocks <= data.size() / 64; ++blocks) {
    llmcc::sha256_detail::State portable = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    llmcc::sha256_detail::State dispatched = portable;
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    llmcc::sha256_detail::CompressPortable(portable, bytes, blocks);
    llmcc::sha256_detail::Compress(dispatched, bytes, blocks);
    llmcc::test::Expect(portable == dispatched,
                        "dispatched compression matches the portable one");
  }
  const std::string reference = HashInChunks(data, data.size());
  for (std::size_t chunk = 1; chunk <= 130; ++chunk) {
    llmcc::test::ExpectEq(HashInChunks(data, chunk), reference,
                          "every chunk size yields the same digest");
  }
  return 0;
}
