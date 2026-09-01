#ifndef LLM_CC_PROJECT_H_
#define LLM_CC_PROJECT_H_

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "src/lang.h"

namespace llmcc {

struct DiscoveredSource {
  std::filesystem::path path;
  Language language;
  std::optional<std::filesystem::path> repository;
};

struct DiscoveryOptions {
  std::optional<Language> language;
  bool include_headers = false;
  bool no_ignore = false;
};

struct DiscoveryResult {
  std::vector<DiscoveredSource> sources;
  std::vector<std::string> warnings;
};

std::optional<std::filesystem::path> FindGitRepository(
    const std::filesystem::path& path);
DiscoveryResult DiscoverSources(
    const std::vector<std::filesystem::path>& inputs,
    const DiscoveryOptions& options = {});

}  // namespace llmcc

#endif  // LLM_CC_PROJECT_H_
