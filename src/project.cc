#include "src/project.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#include "src/git.h"

namespace llmcc {
namespace {

std::string PathUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::u8string value = path.u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
#else
  return path.generic_string();
#endif
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
#if defined(_WIN32)
  const auto path_size = std::distance(path.begin(), path.end());
  const auto directory_size = std::distance(directory.begin(), directory.end());
  if (path_size < directory_size) {
    return false;
  }
  std::filesystem::path ancestor = path;
  for (auto count = path_size; count > directory_size; --count) {
    ancestor = ancestor.parent_path();
  }
  std::error_code error;
  return std::filesystem::equivalent(ancestor, directory, error) && !error;
#else
  auto path_iterator = path.begin();
  for (auto iterator = directory.begin(); iterator != directory.end();
       ++iterator, ++path_iterator) {
    if (path_iterator == path.end() || *path_iterator != *iterator) {
      return false;
    }
  }
  return true;
#endif
}

#if defined(_WIN32)
bool IsDirectoryReparsePoint(const std::filesystem::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
#endif
// Git metadata and llm-cc caches are never analyzed, even when named.
bool InFixedExclusion(const std::filesystem::path& path) {
  return std::ranges::any_of(path, [](const auto& component) {
    return component == ".git" || component == ".llm-cc-cache";
  });
}

// The '/'-separated spelling rules match, whatever the host separator.
std::string GenericUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::u8string value = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
#else
  return path.generic_string();
#endif
}

std::string RelativeUtf8(const std::filesystem::path& path,
                         const std::filesystem::path& root) {
  return GenericUtf8(path.lexically_relative(root));
}

// A regular pyvenv.cfg marks a virtual environment; a symlinked marker does
// not, as in a committed tree where the link is not a regular file.
bool PythonVirtualEnvironment(const std::filesystem::path& directory) {
  std::error_code error;
  return std::filesystem::symlink_status(directory / "pyvenv.cfg", error)
                 .type() == std::filesystem::file_type::regular &&
         !error;
}

bool PythonVirtualEnvironmentPath(const std::filesystem::path& path,
                                  const std::filesystem::path& root) {
  if (PythonVirtualEnvironment(root)) {
    return true;
  }
  std::filesystem::path directory = root;
  for (const auto& component : path.lexically_relative(root)) {
    directory /= component;
    if (PythonVirtualEnvironment(directory)) {
      return true;
    }
  }
  return false;
}

// Loads each Git root's rules once; outside Git the built-ins apply.
class RulesCache {
 public:
  const Rules& For(const std::optional<std::filesystem::path>& repository) {
    if (!repository.has_value()) {
      if (!builtin_used_) {
        builtin_used_ = true;
        sources_.push_back({.repository = std::nullopt, .path = std::nullopt});
      }
      return Rules::Builtin();
    }
    auto found = loaded_.find(*repository);
    if (found == loaded_.end()) {
      LoadedRules loaded;
      try {
        loaded = Rules::LoadFromWorktree(*repository);
      } catch (const RulesError& error) {
        throw std::runtime_error("invalid rules in " + PathUtf8(*repository) +
                                 ": " + error.what());
      }
      sources_.push_back({.repository = repository, .path = loaded.path});
      found = loaded_.emplace(*repository, std::move(loaded)).first;
    }
    return found->second.rules;
  }

  std::vector<RulesSource> TakeSources() { return std::move(sources_); }

 private:
  std::map<std::filesystem::path, LoadedRules> loaded_;
  std::vector<RulesSource> sources_;
  bool builtin_used_ = false;
};

struct WalkContext {
  const DiscoveryOptions& options;
  RulesCache& rules;
  std::map<std::string, DiscoveredSource>& files;
};

// `relative` is the rules path; explicit inputs bypass exclusion rules but
// still resolve their language and category through them.
void AddFile(const std::filesystem::path& path, std::string relative,
             bool explicit_file, const Rules& rules,
             const std::optional<std::filesystem::path>& repository,
             WalkContext& context) {
  const std::filesystem::path canonical = Canonical(path);
  // Within a worktree only its own metadata counts; a directory above the
  // root may be named .git. Git metadata itself has no worktree, so outside
  // one the whole path is checked.
  if (repository.has_value() ? AlwaysExcluded(relative)
                             : InFixedExclusion(canonical)) {
    return;
  }
  std::optional<Language> language = rules.ResolveLanguage(relative);
  // A forced language changes how selected files are parsed; it selects
  // unsupported files only when they are named explicitly.
  if (context.options.language.has_value() &&
      (language.has_value() || explicit_file)) {
    language = context.options.language;
  }
  if (!language.has_value()) {
    if (explicit_file) {
      // Reports the unsupported extension and how to force a language.
      static_cast<void>(InferLanguage(PathUtf8(canonical)));
    }
    return;
  }
  const Category category = rules.Classify(relative);
  context.files.emplace(PathUtf8(canonical),
                        DiscoveredSource{.path = canonical,
                                         .language = *language,
                                         .repository = repository,
                                         .relative_path = std::move(relative),
                                         .category = category});
}

bool SkipDirectory(const std::filesystem::path& path, std::string_view relative,
                   const Rules& rules, bool no_ignore) {
  const std::string name = PathUtf8(path.filename());
  if (name == ".git" || name == ".llm-cc-cache") {
    return true;
  }
#if defined(_WIN32)
  if (IsDirectoryReparsePoint(path)) {
    return true;
  }
#endif
  if (no_ignore) {
    return false;
  }
  return rules.PrunesDirectory(relative) || PythonVirtualEnvironment(path);
}

bool GitWalk(const std::filesystem::path& input,
             const std::filesystem::path& repository, WalkContext& context);

// A directory that is itself the root of another Git worktree.
std::optional<std::filesystem::path> NestedRepositoryRoot(
    const std::filesystem::path& directory,
    const std::optional<std::filesystem::path>& enclosing) {
  std::error_code error;
  if (!std::filesystem::exists(directory / ".git", error) || error) {
    return std::nullopt;
  }
  const auto root = FindGitRepository(directory);
  const auto canonical = std::filesystem::canonical(directory, error);
  if (!root.has_value() || error || *root != canonical || root == enclosing) {
    return std::nullopt;
  }
  return root;
}

void FilesystemWalk(const std::filesystem::path& directory,
                    const std::optional<std::filesystem::path>& repository,
                    WalkContext& context) {
  const bool no_ignore = context.options.no_ignore;
  if (!no_ignore && PythonVirtualEnvironment(directory)) {
    return;
  }
  const Rules& rules = context.rules.For(repository);
  const std::filesystem::path& root = repository.value_or(directory);
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
    const bool directory_entry = entry.is_directory(error);
    if (error) {
      error.clear();
      iterator.increment(error);
      continue;
    }
    std::string relative = RelativeUtf8(entry.path(), root);
    if (directory_entry) {
      if (SkipDirectory(entry.path(), relative, rules, no_ignore)) {
        iterator.disable_recursion_pending();
      } else if (const auto nested =
                     NestedRepositoryRoot(entry.path(), repository)) {
        // A nested repository is discovered with its own rules.
        iterator.disable_recursion_pending();
        if (!GitWalk(entry.path(), *nested, context)) {
          FilesystemWalk(entry.path(), nested, context);
        }
      }
    } else if (!entry.is_symlink(error) && entry.is_regular_file(error) &&
               !error) {
      const auto canonical = std::filesystem::canonical(entry.path(), error);
      if (!error && IsWithin(canonical, directory) &&
          (no_ignore || !rules.Excluded(relative))) {
        AddFile(canonical, std::move(relative), false, rules, repository,
                context);
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
             const std::filesystem::path& repository, WalkContext& context) {
  const bool no_ignore = context.options.no_ignore;
  const auto listed = git::ListFiles(repository, no_ignore);
  if (!listed.has_value()) {
    return false;
  }
  const Rules& rules = context.rules.For(repository);
  std::set<std::filesystem::path> nested_repositories;
  for (const std::string& relative : *listed) {
    if (AlwaysExcluded(relative)) {
      continue;
    }
    const std::filesystem::path candidate =
        repository / std::filesystem::u8path(relative);
    std::error_code error;
    // A symlink would lend its own path, and so its language and rules, to
    // its target; the target is discovered through its own entry.
    if (std::filesystem::is_symlink(candidate, error)) {
      continue;
    }
    const auto canonical = std::filesystem::canonical(candidate, error);
    if (error || !IsWithin(canonical, input)) {
      continue;
    }
    const bool directory = std::filesystem::is_directory(canonical, error);
    if (error) {
      continue;
    }
    if (!directory) {
      if (std::filesystem::is_regular_file(canonical) &&
          (no_ignore ||
           (!rules.Excluded(relative) &&
            !PythonVirtualEnvironmentPath(canonical, repository)))) {
        AddFile(canonical, relative, false, rules, repository, context);
      }
      continue;
    }
    // A directory entry is a submodule or an untracked nested repository,
    // which its own rules govern once the parent's rules admit it.
    const auto nested_repository = FindGitRepository(canonical);
    if ((!no_ignore && (rules.PrunesDirectory(relative) ||
                        PythonVirtualEnvironmentPath(canonical, repository))) ||
        !nested_repository.has_value() || *nested_repository == repository ||
        !nested_repositories.insert(*nested_repository).second) {
      continue;
    }
    if (!GitWalk(canonical, *nested_repository, context)) {
      FilesystemWalk(canonical, nested_repository, context);
    }
  }
  return true;
}

}  // namespace

std::optional<std::filesystem::path> FindGitRepository(
    const std::filesystem::path& path) {
  return git::FindRepositoryRoot(path);
}

DiscoveryResult DiscoverSources(
    const std::vector<std::filesystem::path>& inputs,
    const DiscoveryOptions& options) {
  if (inputs.empty()) {
    throw std::invalid_argument("at least one input path is required");
  }
  DiscoveryResult result;
  std::map<std::string, DiscoveredSource> files;
  RulesCache rules;
  WalkContext context{.options = options, .rules = rules, .files = files};
  for (const auto& raw_input : inputs) {
    const auto input = Canonical(raw_input);
    std::error_code error;
    const bool regular = std::filesystem::is_regular_file(input, error);
    if (error) {
      throw std::runtime_error("cannot inspect input " + PathUtf8(input) +
                               ": " + error.message());
    }
    const auto repository = FindGitRepository(input);
    if (regular) {
      std::string relative = repository.has_value()
                                 ? RelativeUtf8(input, *repository)
                                 : GenericUtf8(input.filename());
      AddFile(input, std::move(relative), true, rules.For(repository),
              repository, context);
      continue;
    }
    if (!std::filesystem::is_directory(input, error) || error) {
      throw std::invalid_argument("input is not a regular file or directory: " +
                                  input.string());
    }
    if (repository.has_value()) {
      if (GitWalk(input, *repository, context)) {
        continue;
      }
      result.warnings.push_back("Git discovery failed for " + PathUtf8(input) +
                                "; falling back to filesystem discovery");
    } else {
      result.warnings.push_back("Git is unavailable or " + PathUtf8(input) +
                                " is not in a Git worktree; falling back to "
                                "filesystem discovery");
    }
    FilesystemWalk(input, repository, context);
  }
  for (auto& [key, source] : files) {
    static_cast<void>(key);
    result.sources.push_back(std::move(source));
  }
  result.rules = rules.TakeSources();
  return result;
}

}  // namespace llmcc
