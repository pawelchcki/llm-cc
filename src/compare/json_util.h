#ifndef LLM_CC_COMPARE_JSON_UTIL_H_
#define LLM_CC_COMPARE_JSON_UTIL_H_

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace llmcc::compare {

// The canonical encoding every comparison artifact and digest uses: sorted
// keys, no whitespace, ASCII-only escapes, and shortest round-trip floats
// spelled like Python's repr (`2.0`, `1e-05`). It is byte-compatible with
// json.dumps(sort_keys=True, separators=(",", ":"), ensure_ascii=True).
// Throws std::invalid_argument for non-finite numbers or invalid UTF-8.
std::string CanonicalJson(const nlohmann::json& value);

// SHA-256 of the canonical encoding.
std::string CanonicalDigest(const nlohmann::json& value);

// Parses a JSON file, rejecting non-finite numbers.
nlohmann::json ReadJsonFile(const std::filesystem::path& path);

// Atomically replaces `path` with `contents`, creating parent directories.
void WriteFileAtomic(const std::filesystem::path& path,
                     std::string_view contents);
// Writes the canonical encoding followed by a newline.
void WriteJsonFile(const std::filesystem::path& path,
                   const nlohmann::json& value);

// JSON number kinds, excluding booleans.
bool IsInteger(const nlohmann::json& value);
bool IsNumber(const nlohmann::json& value);
// Python truthiness: null, false, zero and empty containers are false.
bool Truthy(const nlohmann::json& value);

std::string ReadFileBytes(const std::filesystem::path& path);

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_JSON_UTIL_H_
