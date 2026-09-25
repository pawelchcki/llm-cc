#include "src/rules_cmd.h"

#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "src/input_limits.h"
#include "src/lang.h"
#include "src/progress.h"
#include "src/project.h"
#include "src/rules.h"

namespace llmcc {
namespace {

constexpr std::string_view kUsage =
    "Usage:\n"
    "  llm-cc rules show [PATH]\n"
    "  llm-cc rules check FILE\n"
    "  llm-cc rules explain PATH... [--format text|json]\n\n"
    "show prints the effective rules for the Git worktree containing PATH\n"
    "(default: the current directory). check validates a rules file and\n"
    "prints its effective rules. explain reports how each path would be\n"
    "selected, which language scores it, and its category. .gitignore and\n"
    "Python virtual environments are not consulted.\n";

class RulesUsageError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

std::string PathUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::u8string value = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
#else
  return path.generic_string();
#endif
}

nlohmann::json OptionalJson(const std::optional<std::string>& value) {
  return value.has_value() ? nlohmann::json(*value) : nlohmann::json();
}

struct WorktreeRules {
  std::optional<std::filesystem::path> repository;
  LoadedRules loaded;
};

// Explained paths need not exist yet, so the nearest existing ancestor names
// their worktree.
std::filesystem::path ExistingAncestor(std::filesystem::path path) {
  std::error_code error;
  while (!std::filesystem::exists(path, error) && path.has_relative_path()) {
    path = path.parent_path();
  }
  return path;
}

WorktreeRules RulesFor(const std::filesystem::path& path) {
  WorktreeRules result{.repository = FindGitRepository(ExistingAncestor(path)),
                       .loaded = {.rules = Rules::Builtin(), .path = {}}};
  if (result.repository.has_value()) {
    result.loaded = Rules::LoadFromWorktree(*result.repository);
  }
  return result;
}

int Show(const std::vector<std::string_view>& arguments) {
  if (arguments.size() > 1) {
    throw RulesUsageError("rules show takes at most one PATH");
  }
  const std::filesystem::path path =
      arguments.empty() ? std::filesystem::current_path()
                        : std::filesystem::u8path(arguments[0]);
  const WorktreeRules rules = RulesFor(std::filesystem::absolute(path));
  const nlohmann::json output = {
      {"repository", rules.repository.has_value()
                         ? nlohmann::json(PathUtf8(*rules.repository))
                         : nlohmann::json()},
      {"path", OptionalJson(rules.loaded.path)},
      {"rules", rules.loaded.rules.ToJson()}};
  std::cout << output.dump(2) << '\n';
  return 0;
}

int Check(const std::vector<std::string_view>& arguments) {
  if (arguments.size() != 1) {
    throw RulesUsageError("rules check takes exactly one FILE");
  }
  const std::filesystem::path path = std::filesystem::u8path(arguments[0]);
  std::string text;
  try {
    text = ReadBoundedFile(path, kMaxRulesBytes);
  } catch (const std::length_error&) {
    throw RulesError("classification rules exceed 64 KiB");
  }
  std::cout << Rules::Parse(text).ToJson().dump(2) << '\n';
  return 0;
}

struct Explanation {
  std::string input;
  std::optional<std::filesystem::path> repository;
  std::optional<std::string> rules_path;
  std::string relative;
  std::optional<std::string_view> reason;
  std::optional<Language> language;
  Category category = Category::kRuntime;
};

Explanation Explain(std::string_view input) {
  const std::filesystem::path given = std::filesystem::u8path(input);
  std::filesystem::path absolute =
      std::filesystem::absolute(given).lexically_normal();
  // Discovery analyzes an explicit symlink as its target and skips one found
  // in a directory, so explain the file analysis would actually read.
  std::error_code link_error;
  const bool symlink = std::filesystem::is_symlink(absolute, link_error);
  if (symlink) {
    const auto target = std::filesystem::canonical(absolute, link_error);
    if (!link_error) {
      absolute = target;
    }
  }
  const WorktreeRules worktree = RulesFor(absolute);
  Explanation result{.input = std::string(input),
                     .repository = worktree.repository,
                     .rules_path = worktree.loaded.path};
  if (worktree.repository.has_value()) {
    // Resolve the directories that spell the worktree root, not the path
    // itself: Git sees a symbolic link by its own name.
    std::error_code error;
    const auto directory =
        std::filesystem::weakly_canonical(absolute.parent_path(), error);
    result.relative = PathUtf8(
        ((error ? absolute.parent_path() : directory) / absolute.filename())
            .lexically_relative(*worktree.repository));
  } else {
    result.relative =
        PathUtf8(symlink ? absolute.filename() : given.lexically_normal());
  }
  const Rules& rules = worktree.loaded.rules;
  result.language = rules.ResolveLanguage(result.relative);
  result.category = rules.Classify(result.relative);
  if (AlwaysExcluded(result.relative)) {
    result.reason = "always-excluded";
  } else if (rules.Excluded(result.relative)) {
    result.reason = "excluded";
  } else if (!result.language.has_value()) {
    result.reason = "unsupported";
  }
  return result;
}

int ExplainAll(const std::vector<std::string_view>& arguments) {
  std::vector<std::string_view> paths;
  std::string_view format = "text";
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (arguments[index] == "--format") {
      if (++index >= arguments.size()) {
        throw RulesUsageError("--format requires text or json");
      }
      format = arguments[index];
      if (format != "text" && format != "json") {
        throw RulesUsageError("--format expects text or json");
      }
    } else {
      paths.push_back(arguments[index]);
    }
  }
  if (paths.empty()) {
    throw RulesUsageError("rules explain requires at least one PATH");
  }
  for (const std::string_view path : paths) {
    const Explanation explanation = Explain(path);
    const std::string language =
        explanation.language.has_value()
            ? std::string(LanguageName(*explanation.language))
            : std::string();
    const std::string rules_source =
        explanation.rules_path.value_or("built-in");
    if (format == "json") {
      const nlohmann::json event = {
          {"path", explanation.input},
          {"relative_path", explanation.relative},
          {"repository", explanation.repository.has_value()
                             ? nlohmann::json(PathUtf8(*explanation.repository))
                             : nlohmann::json()},
          {"rules", OptionalJson(explanation.rules_path)},
          {"selected", !explanation.reason.has_value()},
          {"reason", explanation.reason.has_value()
                         ? nlohmann::json(*explanation.reason)
                         : nlohmann::json()},
          {"language",
           language.empty() ? nlohmann::json() : nlohmann::json(language)},
          {"category", CategoryName(explanation.category)}};
      std::cout << event.dump() << '\n';
    } else {
      std::cout << TerminalSafe(explanation.input) << '\t'
                << (explanation.reason.has_value() ? *explanation.reason
                                                   : "selected")
                << '\t' << (language.empty() ? "-" : language) << '\t'
                << CategoryName(explanation.category) << '\t'
                << TerminalSafe(rules_source) << '\n';
    }
  }
  return 0;
}

}  // namespace

int RunRulesCommand(int argc, char** argv) {
  std::vector<std::string_view> arguments(argv + 1, argv + argc);
  if (arguments.empty() || arguments[0] == "-h" || arguments[0] == "--help") {
    std::cerr << kUsage;
    return arguments.empty() ? 2 : 0;
  }
  const std::string_view action = arguments[0];
  arguments.erase(arguments.begin());
  try {
    if (action == "show") {
      return Show(arguments);
    }
    if (action == "check") {
      return Check(arguments);
    }
    if (action == "explain") {
      return ExplainAll(arguments);
    }
    throw RulesUsageError("rules requires show, check, or explain");
  } catch (const RulesUsageError& error) {
    std::cerr << "error: " << error.what() << "\n\n" << kUsage;
    return 2;
  } catch (const RulesError& error) {
    std::cerr << "error: " << TerminalSafe(error.what()) << '\n';
    return 1;
  }
}

}  // namespace llmcc
