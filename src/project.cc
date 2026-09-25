#include "src/project.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

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

#if defined(_WIN32)
std::wstring WindowsArgument(std::wstring_view value) {
  std::wstring result = L"\"";
  std::size_t backslashes = 0;
  for (wchar_t character : value) {
    if (character == L'\\') {
      ++backslashes;
    } else if (character == L'"') {
      result.append(backslashes * 2 + 1, L'\\');
      result += character;
      backslashes = 0;
    } else {
      result.append(backslashes, L'\\');
      backslashes = 0;
      result += character;
    }
  }
  result.append(backslashes * 2, L'\\');
  return result + L'"';
}
#else
std::string ShellQuote(std::string_view value) {
  std::string result = "'";
  for (char character : value) {
    result += character == '\'' ? "'\\''" : std::string(1, character);
  }
  return result + "'";
}

std::string_view NullRedirect() { return " 2>/dev/null"; }
#endif

struct CommandResult {
  int status;
  std::string output;
};

#if defined(_WIN32)
CommandResult Capture(std::wstring_view arguments) {
  const DWORD path_size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
  if (path_size == 0) {
    return {.status = -1, .output = {}};
  }
  std::vector<wchar_t> search_path(path_size);
  if (GetEnvironmentVariableW(L"PATH", search_path.data(), path_size) == 0) {
    return {.status = -1, .output = {}};
  }
  const DWORD executable_size =
      SearchPathW(search_path.data(), L"git.exe", nullptr, 0, nullptr, nullptr);
  if (executable_size == 0) {
    return {.status = -1, .output = {}};
  }
  std::vector<wchar_t> executable(executable_size + 1);
  if (SearchPathW(search_path.data(), L"git.exe", nullptr,
                  static_cast<DWORD>(executable.size()), executable.data(),
                  nullptr) == 0) {
    return {.status = -1, .output = {}};
  }
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &security, 0) ||
      !SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
    if (read_pipe) CloseHandle(read_pipe);
    if (write_pipe) CloseHandle(write_pipe);
    return {.status = -1, .output = {}};
  }
  HANDLE null_error =
      CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = write_pipe;
  startup.hStdError = null_error;
  PROCESS_INFORMATION process{};
  std::wstring mutable_command =
      WindowsArgument(executable.data()) + L" " + std::wstring(arguments);
  const BOOL started = CreateProcessW(executable.data(), mutable_command.data(),
                                      nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                      nullptr, nullptr, &startup, &process);
  CloseHandle(write_pipe);
  if (null_error != INVALID_HANDLE_VALUE) CloseHandle(null_error);
  if (!started) {
    CloseHandle(read_pipe);
    return {.status = -1, .output = {}};
  }
  std::string output;
  std::array<char, 4096> buffer{};
  DWORD count = 0;
  while (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()),
                  &count, nullptr) &&
         count != 0) {
    output.append(buffer.data(), count);
  }
  CloseHandle(read_pipe);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD status = static_cast<DWORD>(-1);
  GetExitCodeProcess(process.hProcess, &status);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return {.status = static_cast<int>(status), .output = std::move(output)};
}
#else
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
#endif
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
bool InCacheDirectory(const std::filesystem::path& path) {
  return std::ranges::any_of(
      path, [](const auto& component) { return component == ".llm-cc-cache"; });
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

bool PythonVirtualEnvironment(const std::filesystem::path& directory) {
  std::error_code error;
  return std::filesystem::is_regular_file(directory / "pyvenv.cfg", error) &&
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
  if (InCacheDirectory(canonical)) {
    return;
  }
  std::optional<Language> language = context.options.language;
  if (!language.has_value()) {
    language = rules.ResolveLanguage(relative);
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
      }
    } else if (entry.is_regular_file(error) && !error) {
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
#if defined(_WIN32)
  std::wstring command = L"-C " + WindowsArgument(repository.native()) +
                         L" ls-files --cached --others" +
                         (no_ignore ? L"" : L" --exclude-standard") + L" -z";
#else
  std::string command = "git -C " + ShellQuote(repository.native()) +
                        " ls-files --cached --others" +
                        (no_ignore ? "" : " --exclude-standard") + " -z" +
                        std::string(NullRedirect());
#endif
  const CommandResult result = Capture(command);
  if (result.status != 0) {
    return false;
  }
  const Rules& rules = context.rules.For(repository);
  std::set<std::filesystem::path> nested_repositories;
  std::string_view remaining = result.output;
  while (!remaining.empty()) {
    const std::size_t end = remaining.find('\0');
    const std::string_view relative = remaining.substr(0, end);
    remaining = end == std::string_view::npos ? std::string_view{}
                                              : remaining.substr(end + 1);
    if (relative.empty() || AlwaysExcluded(relative)) {
      continue;
    }
    const std::filesystem::path candidate =
        repository / std::filesystem::u8path(relative);
    std::error_code error;
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
        AddFile(canonical, std::string(relative), false, rules, repository,
                context);
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
  std::error_code error;
  std::filesystem::path probe =
      std::filesystem::is_directory(path, error) ? path : path.parent_path();
  if (error || probe.empty()) {
    return std::nullopt;
  }
#if defined(_WIN32)
  const CommandResult result = Capture(
      L"-C " + WindowsArgument(probe.native()) + L" rev-parse --show-toplevel");
#else
  const CommandResult result =
      Capture("git -C " + ShellQuote(probe.native()) +
              " rev-parse --show-toplevel" + std::string(NullRedirect()));
#endif
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
  const auto canonical =
      std::filesystem::canonical(std::filesystem::u8path(root), error);
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
