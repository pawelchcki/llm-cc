#ifndef LLM_CC_PROJECT_H_
#define LLM_CC_PROJECT_H_

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "src/lang.h"
#include "src/rules.h"

namespace llmcc {

struct DiscoveredSource {
  std::filesystem::path path;
  Language language;
  std::optional<std::filesystem::path> repository;
  // '/'-separated path the rules matched: relative to the Git root, or to
  // the walked directory outside Git.
  std::string relative_path;
  Category category = Category::kRuntime;
};

struct DiscoveryOptions {
  std::optional<Language> language;
  // Selects ignored and excluded files too. Git metadata, llm-cc's cache and
  // language resolution still apply.
  bool no_ignore = false;
};

// The rules one discovery root used.
struct RulesSource {
  std::optional<std::filesystem::path> repository;
  // The repository-relative rules file, or nullopt for the built-ins.
  std::optional<std::string> path;
};

struct DiscoveryResult {
  std::vector<DiscoveredSource> sources;
  std::vector<std::string> warnings;
  std::vector<RulesSource> rules;
};

std::optional<std::filesystem::path> FindGitRepository(
    const std::filesystem::path& path);
DiscoveryResult DiscoverSources(
    const std::vector<std::filesystem::path>& inputs,
    const DiscoveryOptions& options = {});

}  // namespace llmcc

#endif  // LLM_CC_PROJECT_H_
