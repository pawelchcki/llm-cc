#include "src/rules.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "src/input_limits.h"

namespace llmcc {
namespace {

// Generated and dependency trees analysis skips unless a rules file says
// otherwise. `.git` and `.llm-cc-cache` are fixed exclusions, not rules.
constexpr std::initializer_list<std::string_view> kDefaultExclude = {
    "**/.hg/**",
    "**/.svn/**",
    "**/target/**",
    "**/node_modules/**",
    "**/.gradle/**",
    "**/.venv/**",
    "**/__pycache__/**",
    "**/.tox/**",
    "**/.nox/**",
    "**/.mypy_cache/**",
    "**/.pytest_cache/**",
    "**/.ruff_cache/**",
    "**/vendor/**",
    "**/third_party/**",
    "**/build/**",
    "**/build-out/**",
    "**/.nuget/**",
    "**/dist/**",
    "**/deps/**",
    "**/_build/**",
    "**/cmake-build-debug/**",
    "**/cmake-build-release/**",
    "**/bazel-*/**",
    "out/**",
    "**/bin/**/*.cs",
    "**/obj/**/*.cs",
};

constexpr std::initializer_list<std::string_view> kDefaultTests = {
    "**/test/**", "**/tests/**",   "**/testdata/**", "**/fixtures/**",
    "**/fuzz/**", "**/fuzzers/**", "**/*_test.*",    "**/test_*.*",
};

constexpr std::initializer_list<std::string_view> kDefaultTooling = {
    "tools/**", "examples/**", "example/**", "scripts/**", "benchmarks/**",
};

constexpr std::initializer_list<std::string_view> kKeys = {
    "exclude", "tests", "tooling", "extensions", "paths"};

std::vector<Glob> CompileAll(std::initializer_list<std::string_view> patterns) {
  std::vector<Glob> globs;
  globs.reserve(patterns.size());
  for (const std::string_view pattern : patterns) {
    globs.push_back(Glob::Compile(pattern));
  }
  return globs;
}

bool AnyMatches(const std::vector<Glob>& globs, std::string_view path) {
  return std::ranges::any_of(
      globs, [path](const Glob& glob) { return glob.Matches(path); });
}

nlohmann::json PatternsJson(const std::vector<Glob>& globs) {
  nlohmann::json patterns = nlohmann::json::array();
  for (const Glob& glob : globs) {
    patterns.push_back(glob.pattern());
  }
  return patterns;
}

Glob CompileRulePattern(std::string_view name, const nlohmann::json& value) {
  if (!value.is_string() || value.get_ref<const std::string&>().empty() ||
      value.get_ref<const std::string&>().size() > kMaxRulePatternLength) {
    throw RulesError("classification rule " + std::string(name) +
                     " needs non-empty globs of at most " +
                     std::to_string(kMaxRulePatternLength) + " characters");
  }
  try {
    return Glob::Compile(value.get_ref<const std::string&>());
  } catch (const std::invalid_argument& error) {
    throw RulesError("classification rule " + std::string(name) + ": " +
                     error.what());
  }
}

std::vector<Glob> ParsePatterns(const nlohmann::json& rules,
                                std::string_view name, std::size_t& total) {
  const nlohmann::json& patterns = rules.at(std::string(name));
  if (!patterns.is_array()) {
    throw RulesError("classification rule " + std::string(name) +
                     " must be a list of globs");
  }
  total += patterns.size();
  std::vector<Glob> globs;
  globs.reserve(patterns.size());
  for (const nlohmann::json& pattern : patterns) {
    globs.push_back(CompileRulePattern(name, pattern));
  }
  return globs;
}

// Rules name languages canonically so that plans and reports have one
// spelling for each.
Language ParseCanonicalLanguage(const nlohmann::json& value,
                                std::string_view context) {
  if (value.is_string()) {
    const auto& name = value.get_ref<const std::string&>();
    try {
      const Language language = ParseLanguage(name);
      if (LanguageName(language) == name) {
        return language;
      }
    } catch (const std::invalid_argument&) {  // NOLINT(bugprone-empty-catch)
      // Reported below with the rule that named it.
    }
  }
  throw RulesError("classification rule " + std::string(context) +
                   " names an unsupported language");
}

bool ValidExtension(std::string_view extension) {
  if (extension.size() < 2 || extension.size() > 17 || extension[0] != '.') {
    return false;
  }
  return std::ranges::all_of(extension.substr(1), [](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_' ||
           character == '+' || character == '-';
  });
}

std::string_view LastSegment(std::string_view path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

}  // namespace

std::string_view CategoryName(Category category) {
  switch (category) {
    case Category::kTests:
      return "tests";
    case Category::kTooling:
      return "tooling";
    case Category::kRuntime:
      break;
  }
  return "runtime";
}

const Rules& Rules::Builtin() {
  static const Rules rules = [] {
    Rules value;
    value.exclude_ = CompileAll(kDefaultExclude);
    value.tests_ = CompileAll(kDefaultTests);
    value.tooling_ = CompileAll(kDefaultTooling);
    return value;
  }();
  return rules;
}

Rules Rules::Parse(std::string_view text) {
  if (text.size() > kMaxRulesBytes) {
    throw RulesError("classification rules exceed 64 KiB");
  }
  nlohmann::json value;
  try {
    value = nlohmann::json::parse(text);
  } catch (const nlohmann::json::parse_error& error) {
    throw RulesError(std::string("classification rules are not valid JSON: ") +
                     error.what());
  }
  return FromJson(value);
}

Rules Rules::FromJson(const nlohmann::json& value) {
  if (!value.is_object()) {
    throw RulesError("classification rules must be a JSON object");
  }
  std::set<std::string> unknown;
  for (const auto& [key, item] : value.items()) {
    static_cast<void>(item);
    if (std::ranges::find(kKeys, key) == kKeys.end()) {
      unknown.insert(key);
    }
  }
  if (!unknown.empty()) {
    std::string names;
    for (const std::string& key : unknown) {
      names += (names.empty() ? "" : ", ") + key;
    }
    throw RulesError("unsupported classification rule keys: " + names);
  }
  Rules rules = Builtin();
  std::size_t total = 0;
  if (value.contains("exclude")) {
    rules.exclude_ = ParsePatterns(value, "exclude", total);
  }
  if (value.contains("tests")) {
    rules.tests_ = ParsePatterns(value, "tests", total);
  }
  if (value.contains("tooling")) {
    rules.tooling_ = ParsePatterns(value, "tooling", total);
  }
  if (value.contains("paths")) {
    const nlohmann::json& paths = value.at("paths");
    if (!paths.is_array()) {
      throw RulesError("classification rule paths must be a list of objects");
    }
    total += paths.size();
    for (const nlohmann::json& path : paths) {
      if (!path.is_object() || path.size() != 2 || !path.contains("pattern") ||
          !path.contains("language")) {
        throw RulesError(
            "classification rule paths needs {pattern, language} objects");
      }
      Glob pattern = CompileRulePattern("paths", path.at("pattern"));
      const Language language = ParseCanonicalLanguage(
          path.at("language"), "path " + pattern.pattern());
      rules.paths_.push_back(
          PathRule{.pattern = std::move(pattern), .language = language});
    }
  }
  if (value.contains("extensions")) {
    const nlohmann::json& extensions = value.at("extensions");
    if (!extensions.is_object()) {
      throw RulesError("classification rule extensions must be an object");
    }
    for (const auto& [extension, language] : extensions.items()) {
      if (!ValidExtension(extension)) {
        throw RulesError(
            "classification rule extensions must map lowercase .<ext> names");
      }
      rules.extensions_[extension] =
          ParseCanonicalLanguage(language, "extension " + extension);
    }
  }
  if (total > kMaxRulePatterns) {
    throw RulesError("classification rules use more than " +
                     std::to_string(kMaxRulePatterns) + " glob patterns");
  }
  if (value.dump(-1, ' ', true).size() > kMaxRulesBytes) {
    throw RulesError("classification rules exceed 64 KiB");
  }
  return rules;
}

LoadedRules Rules::LoadFromWorktree(const std::filesystem::path& root) {
  for (const std::string_view relative : {kRulesPath, kLegacyRulesPath}) {
    const std::filesystem::path path = root / std::filesystem::path(relative);
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error || status.type() == std::filesystem::file_type::not_found) {
      continue;
    }
    if (status.type() != std::filesystem::file_type::regular) {
      throw RulesError(std::string(relative) + " is not a regular file");
    }
    std::string text;
    try {
      text = ReadBoundedFile(path, kMaxRulesBytes);
    } catch (const std::length_error&) {
      throw RulesError(std::string(relative) +
                       ": classification rules exceed "
                       "64 KiB");
    }
    try {
      return {.rules = Parse(text), .path = std::string(relative)};
    } catch (const RulesError& failure) {
      throw RulesError(std::string(relative) + ": " + failure.what());
    }
  }
  return {.rules = Builtin(), .path = std::nullopt};
}

bool Rules::Excluded(std::string_view path) const {
  return AnyMatches(exclude_, path);
}

bool Rules::PrunesDirectory(std::string_view directory) const {
  return std::ranges::any_of(exclude_, [directory](const Glob& glob) {
    return glob.PrunesDirectory(directory);
  });
}

Category Rules::Classify(std::string_view path) const {
  if (AnyMatches(tests_, path)) {
    return Category::kTests;
  }
  if (AnyMatches(tooling_, path)) {
    return Category::kTooling;
  }
  return Category::kRuntime;
}

std::optional<Language> Rules::ResolveLanguage(std::string_view path) const {
  for (const PathRule& rule : paths_) {
    if (rule.pattern.Matches(path)) {
      return rule.language;
    }
  }
  const std::string extension = PathExtension(path);
  if (const auto configured = extensions_.find(extension);
      configured != extensions_.end()) {
    return configured->second;
  }
  return LanguageForExtension(extension);
}

nlohmann::json Rules::ToJson() const {
  nlohmann::json paths = nlohmann::json::array();
  for (const PathRule& rule : paths_) {
    paths.push_back({{"pattern", rule.pattern.pattern()},
                     {"language", LanguageName(rule.language)}});
  }
  nlohmann::json extensions = nlohmann::json::object();
  for (const auto& [extension, language] : extensions_) {
    extensions[extension] = LanguageName(language);
  }
  return {{"exclude", PatternsJson(exclude_)},
          {"tests", PatternsJson(tests_)},
          {"tooling", PatternsJson(tooling_)},
          {"paths", std::move(paths)},
          {"extensions", std::move(extensions)}};
}

bool AlwaysExcluded(std::string_view path) {
  while (!path.empty()) {
    const std::size_t end = path.find('/');
    const std::string_view segment = path.substr(0, end);
    if (segment == ".git" || segment == ".llm-cc-cache") {
      return true;
    }
    path = end == std::string_view::npos ? std::string_view{}
                                         : path.substr(end + 1);
  }
  return false;
}

std::string PathExtension(std::string_view path) {
  const std::string_view name = LastSegment(path);
  const std::size_t dot = name.rfind('.');
  if (dot == std::string_view::npos) {
    return {};
  }
  std::string extension(name.substr(dot));
  for (char& character : extension) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return extension;
}

}  // namespace llmcc
