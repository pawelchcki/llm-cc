#ifndef LLM_CC_INPUT_LIMITS_H_
#define LLM_CC_INPUT_LIMITS_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <stdexcept>
#include <string>

namespace llmcc {

// Source text must stay addressable by the tokenizer and parser, and bounded
// ingestion prevents an input that grows while being read from evading the
// filesystem-size preflight.
inline constexpr std::uint64_t kMaxSourceBytes = 1024ULL * 1024ULL * 1024ULL;

[[noreturn]] inline void ThrowSourceSizeError(std::uintmax_t maximum) {
  throw std::length_error("source exceeds the maximum supported size of " +
                          std::to_string(maximum) + " bytes");
}

inline void CheckSourceSize(
    std::uintmax_t bytes,
    std::uintmax_t maximum = static_cast<std::uintmax_t>(kMaxSourceBytes)) {
  if (bytes > maximum) {
    ThrowSourceSizeError(maximum);
  }
}

// A size preflight gives regular files a prompt failure. Some inputs (such as
// pipes and files whose metadata cannot be queried) have no reliable size;
// ReadBoundedStream remains the authoritative incremental limit for them.
inline void CheckSourceFileSize(
    const std::filesystem::path& path,
    std::uintmax_t maximum = static_cast<std::uintmax_t>(kMaxSourceBytes)) {
  std::error_code error;
  const std::uintmax_t bytes = std::filesystem::file_size(path, error);
  if (!error) {
    CheckSourceSize(bytes, maximum);
  }
}

inline std::string ReadBoundedStream(
    std::istream& input,
    std::uintmax_t maximum = static_cast<std::uintmax_t>(kMaxSourceBytes)) {
  std::string contents;
  std::array<char, std::size_t{64} * 1024> buffer{};
  for (;;) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize read = input.gcount();
    if (read > 0) {
      const auto count = static_cast<std::uintmax_t>(read);
      if (count > maximum ||
          static_cast<std::uintmax_t>(contents.size()) > maximum - count) {
        ThrowSourceSizeError(maximum);
      }
      contents.append(buffer.data(), static_cast<std::size_t>(read));
    }
    if (input.eof()) {
      break;
    }
    if (!input) {
      throw std::runtime_error("failed while reading source input");
    }
  }
  return contents;
}

inline std::string ReadBoundedFile(
    const std::filesystem::path& path,
    std::uintmax_t maximum = static_cast<std::uintmax_t>(kMaxSourceBytes)) {
  CheckSourceFileSize(path, maximum);
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open input file: " + path.string());
  }
  return ReadBoundedStream(input, maximum);
}

}  // namespace llmcc

#endif  // LLM_CC_INPUT_LIMITS_H_
