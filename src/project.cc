#include "src/project.h"

#include <sys/wait.h>

#include <array>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace llmcc {
namespace {

std::string ShellQuote(std::string_view value) {
  std::string result = "'";
  for (char character : value) {
    result += character == '\'' ? "'\\''" : std::string(1, character);
  }
  return result + "'";
}

struct CommandResult {
  int status;
  std::string output;
};

CommandResult Capture(std::string_view command) {
  using Pipe = std::unique_ptr<std::FILE, decltype(&pclose)>;
  Pipe pipe(popen(std::string(command).c_str(), "r"),  // NOLINT
            pclose);
  if (!pipe) {
    return {.status = -1, .output = {}};
  }
  std::string output;
  std::array<char, 4096> buffer{};
  while (const std::size_t count =
             std::fread(buffer.data(), 1, buffer.size(), pipe.get())) {
    output.append(buffer.data(), count);
  }
  std::FILE* raw = pipe.release();
  const int raw_status = pclose(raw);
  const int status = WIFEXITED(raw_status) ? WEXITSTATUS(raw_status) : -1;
  return {.status = status, .output = std::move(output)};
}

std::filesystem::path Canonical(const std::filesystem::path& path) {
  std::error_code error;
  const auto result = std::filesystem::canonical(path, error);
  if (error) {
    throw std::runtime_error("cannot resolve input " + path.string() + ": " +
                             error.message());
  }
  return result;
}

bool IsWithin(const std::filesystem::path& path,
              const std::filesystem::path& directory) {
  auto path_iterator = path.begin();
  for (auto iterator = directory.begin(); iterator != directory.end();
       ++iterator, ++path_iterator) {
    if (path_iterator == path.end() || *path_iterator != *iterator) {
      return false;
    }
  }
  return true;
}

bool AlwaysExcluded(const std::filesystem::path& path) {
  return std::ranges::any_of(
      path, [](const auto& component) { return component == ".llm-cc-cache"; });
}

bool GeneratedDirectory(std::string_view name) {
  static const std::set<std::string_view> names = {".git",
                                                   ".hg",
                                                   ".svn",
                                                   "target",
                                                   "node_modules",
                                                   ".gradle",
                                                   ".venv",
                                                   "venv",
                                                   "env",
                                                   "__pycache__",
                                                   ".tox",
                                                   ".nox",
                                                   ".mypy_cache",
                                                   ".pytest_cache",
                                                   ".ruff_cache",
                                                   "vendor",
                                                   "third_party",
                                                   "build",
                                                   "build-out",
                                                   "out",
                                                   "bin",
                                                   "obj",
                                                   ".nuget",
                                                   "dist",
                                                   "deps",
                                                   "_build",
                                                   "cmake-build-debug",
                                                   "cmake-build-release"};
  return names.contains(name) || name.starts_with("bazel-");
}

bool GeneratedPath(const std::filesystem::path& path,
                   const std::filesystem::path& repository) {
  const auto relative = path.lexically_relative(repository);
  return std::ranges::any_of(relative, [](const auto& component) {
    return component == ".llm-cc-cache" ||
           GeneratedDirectory(component.string());
  });
}

void AddFile(const std::filesystem::path& path, bool explicit_file,
             const DiscoveryOptions& options,
             const std::optional<std::filesystem::path>& repository,
             std::map<std::string, DiscoveredSource>& files) {
  const std::filesystem::path canonical = Canonical(path);
  if (AlwaysExcluded(canonical)) {
    return;
  }
  if (!explicit_file &&
      !IsSourcePath(canonical.string(), options.include_headers)) {
    return;
  }
  Language language;
  if (options.language.has_value()) {
    language = *options.language;
  } else {
    try {
      language = InferLanguage(canonical.string());
    } catch (const std::invalid_argument&) {
      if (explicit_file) {
        throw;
      }
      return;
    }
  }
  files.emplace(
      canonical.generic_string(),
      DiscoveredSource{
          .path = canonical, .language = language, .repository = repository});
}

void FilesystemWalk(const std::filesystem::path& directory,
                    const DiscoveryOptions& options,
                    const std::optional<std::filesystem::path>& repository,
                    std::map<std::string, DiscoveredSource>& files) {
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(
      directory, std::filesystem::directory_options::skip_permission_denied,
      error);
  const std::filesystem::recursive_directory_iterator end;
  if (error) {
    throw std::runtime_error("cannot read directory " + directory.string() +
                             ": " + error.message());
  }
  while (iterator != end) {
    const auto entry = *iterator;
    const auto name = entry.path().filename().string();
    const bool directory_entry = entry.is_directory(error);
    if (error) {
      error.clear();
      iterator.increment(error);
      continue;
    }
    if (directory_entry &&
        (name == ".llm-cc-cache" ||
         (!options.no_ignore && GeneratedDirectory(name)) || name == ".git")) {
      iterator.disable_recursion_pending();
    } else if (entry.is_regular_file(error) && !error) {
      const auto canonical = std::filesystem::canonical(entry.path(), error);
      if (!error && IsWithin(canonical, directory)) {
        AddFile(canonical, false, options, repository, files);
      }
    }
    error.clear();
    iterator.increment(error);
    if (error) {
      error.clear();
    }
  }
}

bool GitWalk(const std::filesystem::path& input,
             const std::filesystem::path& repository,
             const DiscoveryOptions& options,
             std::map<std::string, DiscoveredSource>& files) {
  std::string command = "git -C " + ShellQuote(repository.string()) +
                        " ls-files --cached --others" +
                        (options.no_ignore ? "" : " --exclude-standard") +
                        " -z 2>/dev/null";
  const CommandResult result = Capture(command);
  if (result.status != 0) {
    return false;
  }
  std::set<std::filesystem::path> nested_repositories;
  std::size_t start = 0;
  while (start < result.output.size()) {
    const std::size_t end = result.output.find('\0', start);
    const std::string_view relative(
        result.output.data() + start,
        (end == std::string::npos ? result.output.size() : end) - start);
    if (!relative.empty()) {
      const std::filesystem::path candidate = repository / relative;
      std::error_code error;
      const auto canonical = std::filesystem::canonical(candidate, error);
      if (!error && IsWithin(canonical, input)) {
        const bool generated = GeneratedPath(canonical, repository);
        if (std::filesystem::is_directory(canonical, error) && !error) {
          const auto nested_repository = FindGitRepository(canonical);
          if ((options.no_ignore || !generated) &&
              nested_repository.has_value() &&
              *nested_repository != repository &&
              nested_repositories.insert(*nested_repository).second) {
            if (!GitWalk(canonical, *nested_repository, options, files)) {
              FilesystemWalk(canonical, options, nested_repository, files);
            }
          }
        } else if (!error && std::filesystem::is_regular_file(canonical) &&
                   (options.no_ignore || !generated)) {
          AddFile(canonical, false, options, repository, files);
        }
      }
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return true;
}

}  // namespace

std::optional<std::filesystem::path> FindGitRepository(
    const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::path probe =
      std::filesystem::is_directory(path, error) ? path : path.parent_path();
  if (error || probe.empty()) {
    return std::nullopt;
  }
  const CommandResult result =
      Capture("git -C " + ShellQuote(probe.string()) +
              " rev-parse --show-toplevel 2>/dev/null");
  if (result.status != 0) {
    return std::nullopt;
  }
  std::string root = result.output;
  while (!root.empty() && (root.back() == '\n' || root.back() == '\r')) {
    root.pop_back();
  }
  if (root.empty()) {
    return std::nullopt;
  }
  const auto canonical = std::filesystem::canonical(root, error);
  return error ? std::nullopt : std::optional<std::filesystem::path>(canonical);
}

DiscoveryResult DiscoverSources(
    const std::vector<std::filesystem::path>& inputs,
    const DiscoveryOptions& options) {
  if (inputs.empty()) {
    throw std::invalid_argument("at least one input path is required");
  }
  DiscoveryResult result;
  std::map<std::string, DiscoveredSource> files;
  for (const auto& raw_input : inputs) {
    const auto input = Canonical(raw_input);
    std::error_code error;
    const bool regular = std::filesystem::is_regular_file(input, error);
    if (error) {
      throw std::runtime_error("cannot inspect input " + input.string() + ": " +
                               error.message());
    }
    const auto repository = FindGitRepository(input);
    if (regular) {
      AddFile(input, true, options, repository, files);
      continue;
    }
    if (!std::filesystem::is_directory(input, error) || error) {
      throw std::invalid_argument("input is not a regular file or directory: " +
                                  input.string());
    }
    if (repository.has_value()) {
      if (GitWalk(input, *repository, options, files)) {
        continue;
      }
      result.warnings.push_back("Git discovery failed for " + input.string() +
                                "; falling back to filesystem discovery");
    } else {
      result.warnings.push_back("Git is unavailable or " + input.string() +
                                " is not in a Git worktree; falling back to "
                                "filesystem discovery");
    }
    FilesystemWalk(input, options, repository, files);
  }
  for (auto& [key, source] : files) {
    static_cast<void>(key);
    result.sources.push_back(std::move(source));
  }
  return result;
}

}  // namespace llmcc
