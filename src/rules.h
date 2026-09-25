#ifndef LLM_CC_RULES_H_
#define LLM_CC_RULES_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "src/glob.h"
#include "src/lang.h"

namespace llmcc {

// Where a repository keeps its selection and classification rules. The legacy
// name predates their use outside CI comparisons and is still read.
inline constexpr std::string_view kRulesPath = ".llm-cc/rules.json";
inline constexpr std::string_view kLegacyRulesPath =
    ".llm-cc/comparison-rules.json";

// Bounds that keep a repository from making selection arbitrarily expensive.
inline constexpr std::size_t kMaxRulesBytes = std::size_t{64} * 1024;
inline constexpr std::size_t kMaxRulePatterns = 512;
inline constexpr std::size_t kMaxRulePatternLength = 256;

enum class Category : std::uint8_t { kRuntime, kTests, kTooling };

inline constexpr std::array kCategories = {Category::kRuntime, Category::kTests,
                                           Category::kTooling};

std::string_view CategoryName(Category category);

class RulesError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

struct LoadedRules;

// One rules engine decides which repository paths are source, which language
// scores them, and which category reports them. Paths are '/'-separated and
// relative to the repository root.
//
// A rules file may set any of these keys; an absent key keeps its built-in
// default, and a present one replaces it:
//
//   exclude     globs of paths that are never selected
//   tests       globs of test paths; checked before tooling
//   tooling     globs of tooling paths; everything else is runtime
//   paths       [{pattern, language}], the first match picks the language
//   extensions  {".ext": language}, lowercase extensions over the built-ins
class Rules {
 public:
  static const Rules& Builtin();
  // Throws RulesError for malformed JSON, unknown keys, or exceeded limits.
  static Rules Parse(std::string_view text);
  static Rules FromJson(const nlohmann::json& value);
  // The rules at kRulesPath, else kLegacyRulesPath, else the built-ins.
  static LoadedRules LoadFromWorktree(const std::filesystem::path& root);

  [[nodiscard]] bool Excluded(std::string_view path) const;
  // True when every path beneath `directory` is excluded.
  [[nodiscard]] bool PrunesDirectory(std::string_view directory) const;
  [[nodiscard]] Category Classify(std::string_view path) const;
  // Path overrides, then configured extensions, then built-in extensions.
  // Extensions compare case-insensitively.
  [[nodiscard]] std::optional<Language> ResolveLanguage(
      std::string_view path) const;
  // The complete effective rules, defaults included.
  [[nodiscard]] nlohmann::json ToJson() const;

 private:
  struct PathRule {
    Glob pattern;
    Language language;
  };
  std::vector<Glob> exclude_;
  std::vector<Glob> tests_;
  std::vector<Glob> tooling_;
  std::vector<PathRule> paths_;
  std::map<std::string, Language, std::less<>> extensions_;
};

struct LoadedRules {
  Rules rules;
  // The repository-relative rules file, or nullopt for the built-in rules.
  std::optional<std::string> path;
};

// Paths no rules file can select: Git metadata and llm-cc's own cache.
bool AlwaysExcluded(std::string_view path);

// The lowercase extension of a path's final segment, including its dot.
std::string PathExtension(std::string_view path);

}  // namespace llmcc

#endif  // LLM_CC_RULES_H_
