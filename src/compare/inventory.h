#ifndef LLM_CC_COMPARE_INVENTORY_H_
#define LLM_CC_COMPARE_INVENTORY_H_

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "src/git.h"
#include "src/rules.h"

namespace llmcc::compare {

// Every path of a commit's tree as a plan inventory entry:
//   {path, language, category, size, blob_id, key, scorable, reason}
// `reason` explains an unscorable path: submodule, symlink, unsupported,
// excluded or oversized. Scorable entries carry their result key.
nlohmann::json BuildInventory(const git::Repository& repository,
                              std::string_view commit, const Rules& rules,
                              std::uint64_t max_file_bytes,
                              std::string_view fingerprint);

// How plans record a Git path, in inventories and changes alike: a
// backslash is spelled \\ and each byte that is not UTF-8 \xHH, so every
// spelling is valid JSON and names exactly one path. A path that is not UTF-8
// is never scored.
std::string PrintablePath(std::string_view path);

struct TargetRules {
  Rules rules;
  // {source: repository, path, commit}, {source: host} or {source: builtin}.
  nlohmann::json source;
};

// The rules committed at the target, so a pull request cannot reclassify its
// own files; otherwise the host's default rules file, otherwise the
// built-ins. Invalid rules throw rather than fall back.
TargetRules LoadTargetRules(
    const git::Repository& repository, std::string_view target_commit,
    const std::optional<std::filesystem::path>& default_rules);

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_INVENTORY_H_
