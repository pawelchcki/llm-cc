#include "src/compare/inventory.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "src/compare/identity.h"
#include "src/input_limits.h"
#include "src/lang.h"

namespace llmcc::compare {
namespace {

using nlohmann::json;

// The length of the well-formed UTF-8 sequence at `index`, or 0.
std::size_t SequenceLength(std::string_view text, std::size_t index) {
  const auto lead = static_cast<unsigned char>(text[index]);
  if (lead < 0x80) {
    return 1;
  }
  std::size_t length = 0;
  char32_t value = 0;
  char32_t minimum = 0;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
    value = lead & 0x1FU;
    minimum = 0x80;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    value = lead & 0x0FU;
    minimum = 0x800;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    value = lead & 0x07U;
    minimum = 0x10000;
  } else {
    return 0;
  }
  if (index + length > text.size()) {
    return 0;
  }
  for (std::size_t offset = 1; offset < length; ++offset) {
    const auto next = static_cast<unsigned char>(text[index + offset]);
    if ((next & 0xC0U) != 0x80U) {
      return 0;
    }
    value = (value << 6U) | (next & 0x3FU);
  }
  const bool valid = value >= minimum && value <= 0x10FFFF &&
                     (value < 0xD800 || value > 0xDFFF);
  return valid ? length : 0;
}

bool ValidUtf8(std::string_view text) {
  for (std::size_t index = 0; index < text.size();) {
    const std::size_t length = SequenceLength(text, index);
    if (length == 0) {
      return false;
    }
    index += length;
  }
  return true;
}

// Plans are JSON, so a path that is not UTF-8 is recorded with its invalid
// bytes spelled \xHH and never scored.
std::string Printable(std::string_view path) {
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string result;
  for (std::size_t index = 0; index < path.size();) {
    const std::size_t length = SequenceLength(path, index);
    if (length != 0) {
      result += path.substr(index, length);
      index += length;
      continue;
    }
    const auto byte = static_cast<unsigned char>(path[index++]);
    result += "\\x";
    result.push_back(kHex[byte >> 4U]);
    result.push_back(kHex[byte & 0xFU]);
  }
  return result;
}

}  // namespace

json BuildInventory(const git::Repository& repository, std::string_view commit,
                    const Rules& rules, std::uint64_t max_file_bytes,
                    std::string_view fingerprint) {
  json entries = json::array();
  for (const git::TreeEntry& entry : repository.ListTree(commit)) {
    const bool valid_path = ValidUtf8(entry.path);
    const std::string path = valid_path ? entry.path : Printable(entry.path);
    const bool blob = entry.type == "blob";
    const std::optional<Language> language =
        blob && valid_path ? rules.ResolveLanguage(path) : std::nullopt;
    json reason;
    if (entry.mode == "160000" || entry.type == "commit") {
      reason = "submodule";
    } else if (entry.mode == "120000") {
      reason = "symlink";
    } else if (!blob || !valid_path) {  // NOLINT(bugprone-branch-clone)
      reason = "unsupported";
    } else if (AlwaysExcluded(path) || rules.Excluded(path)) {
      reason = "excluded";
    } else if (!language.has_value()) {
      reason = "unsupported";
    } else if (entry.size.value_or(0) > max_file_bytes) {
      reason = "oversized";
    }
    const bool scorable = reason.is_null();
    const std::string language_name =
        language.has_value() ? std::string(LanguageName(*language)) : "";
    entries.push_back(
        {{"path", path},
         {"language", language.has_value() ? json(language_name) : json()},
         {"category", CategoryName(rules.Classify(path))},
         {"size", entry.size.has_value() ? json(*entry.size) : json()},
         {"blob_id", blob ? json(entry.object_id) : json()},
         {"key", scorable ? json(ResultKey(entry.object_id, language_name,
                                           fingerprint))
                          : json()},
         {"scorable", scorable},
         {"reason", reason}});
  }
  return entries;
}

TargetRules LoadTargetRules(
    const git::Repository& repository, std::string_view target_commit,
    const std::optional<std::filesystem::path>& default_rules) {
  for (const std::string_view path : {kRulesPath, kLegacyRulesPath}) {
    const std::optional<std::string> text =
        repository.ReadFile(target_commit, path);
    if (!text.has_value()) {
      continue;
    }
    try {
      return {.rules = Rules::Parse(*text),
              .source = {{"source", "repository"},
                         {"path", path},
                         {"commit", target_commit}}};
    } catch (const RulesError& error) {
      throw RulesError("repository rules at " + std::string(path) +
                       " in the target commit are invalid: " + error.what());
    }
  }
  if (default_rules.has_value()) {
    std::string text;
    try {
      text = ReadBoundedFile(*default_rules, kMaxRulesBytes);
    } catch (const std::length_error&) {
      throw RulesError("default rules exceed 64 KiB");
    }
    try {
      return {.rules = Rules::Parse(text), .source = {{"source", "host"}}};
    } catch (const RulesError& error) {
      throw RulesError("default rules are invalid: " +
                       std::string(error.what()));
    }
  }
  return {.rules = Rules::Builtin(), .source = {{"source", "builtin"}}};
}

}  // namespace llmcc::compare
