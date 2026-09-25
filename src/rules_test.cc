#include "src/rules.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "src/test_util.h"

namespace {

using llmcc::Category;
using llmcc::Language;
using llmcc::Rules;

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream text;
  text << input.rdbuf();
  return text.str();
}

void Write(const std::filesystem::path& path, std::string_view value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << value;
}

void ExpectRejected(std::string_view text, std::string_view fragment) {
  try {
    static_cast<void>(Rules::Parse(text));
  } catch (const llmcc::RulesError& error) {
    llmcc::test::Expect(
        std::string(error.what()).find(fragment) != std::string::npos,
        "rules error names the problem: " + std::string(fragment));
    return;
  }
  llmcc::test::Expect(false,
                      "invalid rules are rejected: " + std::string(text));
}

std::string Patterns(std::size_t count) {
  std::string patterns;
  for (std::size_t index = 0; index < count; ++index) {
    patterns += (index == 0 ? "\"a\"" : ",\"a\"");
  }
  return patterns;
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape)
  using llmcc::test::Expect;
  using llmcc::test::ExpectEq;
  const char* test_srcdir = std::getenv("TEST_SRCDIR");
  const char* test_tmpdir = std::getenv("TEST_TMPDIR");
  Expect(argc == 2 && test_srcdir != nullptr && test_tmpdir != nullptr,
         "rules fixture and test directories are provided");
  const std::filesystem::path fixtures =
      (std::filesystem::path(test_srcdir) / argv[1]).parent_path();

  // Built-in selection replaces the old generated-directory list.
  const Rules& builtin = Rules::Builtin();
  Expect(builtin.Excluded("third_party/x.cc") &&
             builtin.Excluded("a/node_modules/b.js") &&
             builtin.Excluded("bazel-out/k8/x.cc") &&
             builtin.Excluded("out/Hidden.java") &&
             builtin.Excluded("bin/Hidden.cs") &&
             builtin.Excluded("src/obj/Debug/Hidden.cs"),
         "built-in rules exclude generated and dependency trees");
  Expect(!builtin.Excluded("src/out/Kept.java") &&
             !builtin.Excluded("bin/tool.csx") &&
             !builtin.Excluded("src/main.cc"),
         "built-in rules keep ordinary sources");
  Expect(builtin.PrunesDirectory("node_modules") &&
             builtin.PrunesDirectory("out") &&
             !builtin.PrunesDirectory("src/out") &&
             !builtin.PrunesDirectory("bin"),
         "built-in rules prune excluded directories only");
  ExpectEq(builtin.Classify("tests/a.cc"), Category::kTests,
           "test directories are tests");
  ExpectEq(builtin.Classify("src/a_test.cc"), Category::kTests,
           "nested test files are tests");
  ExpectEq(builtin.Classify("test_a.py"), Category::kTests,
           "root test files are tests");
  ExpectEq(builtin.Classify("tools/x.py"), Category::kTooling,
           "tool directories are tooling");
  ExpectEq(builtin.Classify("src/tools/x.py"), Category::kRuntime,
           "tooling globs are anchored at the root");
  ExpectEq(builtin.Classify("tools/tests/x.py"), Category::kTests,
           "tests take precedence over tooling");

  // Extensions resolve case-insensitively; headers are ordinary sources.
  ExpectEq(builtin.ResolveLanguage("Foo.PY"), std::optional(Language::kPython),
           "an uppercase Python extension is Python");
  ExpectEq(builtin.ResolveLanguage("src/x.h"), std::optional(Language::kC),
           "a .h header is C by default");
  ExpectEq(builtin.ResolveLanguage("src/X.H"), std::optional(Language::kC),
           "an uppercase .H header is C");
  ExpectEq(builtin.ResolveLanguage("src/y.CPP"), std::optional(Language::kCpp),
           "an uppercase .CPP file is C++");
  ExpectEq(builtin.ResolveLanguage("Main.JAVA"), std::optional(Language::kJava),
           "an uppercase .JAVA file is Java");
  ExpectEq(builtin.ResolveLanguage("README.md"), std::optional<Language>(),
           "unsupported extensions have no language");
  ExpectEq(builtin.ResolveLanguage("Makefile"), std::optional<Language>(),
           "extensionless files have no language");

  // A repository file overrides only the keys it names.
  const Rules valid = Rules::Parse(ReadText(fixtures / "valid.json"));
  Expect(valid.Excluded("gen/a.cc") && valid.Excluded("x/y.pb.cc") &&
             !valid.Excluded("third_party/a.cc"),
         "a configured exclude replaces the built-in excludes");
  ExpectEq(valid.ResolveLanguage("src/x.h"), std::optional(Language::kCpp),
           "a configured extension overrides the built-in table");
  ExpectEq(valid.ResolveLanguage("src/x.INC"), std::optional(Language::kC),
           "configured extensions compare case-insensitively");
  ExpectEq(valid.ResolveLanguage("legacy/new/x.h"),
           std::optional(Language::kRust),
           "the first matching path override wins");
  ExpectEq(valid.ResolveLanguage("legacy/old/x.h"), std::optional(Language::kC),
           "path overrides win over configured extensions");
  const Rules partial = Rules::Parse(R"({"tooling": ["copy.cc"]})");
  ExpectEq(partial.Classify("copy.cc"), Category::kTooling,
           "configured tooling globs apply");
  Expect(partial.Excluded("third_party/a.cc"),
         "an absent key keeps its built-in default");
  ExpectEq(partial.Classify("tests/a.cc"), Category::kTests,
           "absent test globs keep their defaults");
  ExpectEq(Rules::Parse(R"({"tests": ["tests/**"]})").Classify("Tests/main.py"),
           Category::kRuntime, "classification does not fold case");
  ExpectEq(Rules::FromJson(valid.ToJson()).ToJson(), valid.ToJson(),
           "effective rules round-trip through JSON");

  // The repository's own rules stay valid.
  const std::filesystem::path repository_rules =
      fixtures.parent_path().parent_path() / ".llm-cc/rules.json";
  if (std::filesystem::exists(repository_rules)) {
    const Rules own = Rules::Parse(ReadText(repository_rules));
    ExpectEq(own.ResolveLanguage("src/x.h"), std::optional(Language::kCpp),
             "llm-cc scores its headers as C++");
    ExpectEq(own.Classify("src/test_util.h"), Category::kTests,
             "llm-cc test helpers are tests");
  }

  ExpectRejected(R"({"tests": ["ok"], "unexpected": true})",
                 "unsupported classification rule keys: unexpected");
  ExpectRejected("{not json", "not valid JSON");
  ExpectRejected("[]", "must be a JSON object");
  ExpectRejected(R"({"extensions": {".zig": "zig"}})",
                 "names an unsupported language");
  ExpectRejected(R"({"extensions": {".h": "C++"}})",
                 "names an unsupported language");
  ExpectRejected(R"({"extensions": {".H": "cpp"}})", "lowercase .<ext>");
  ExpectRejected(R"({"extensions": {"h": "cpp"}})", "lowercase .<ext>");
  ExpectRejected(R"({"extensions": []})", "must be an object");
  ExpectRejected(R"({"tests": "tests/**"})", "must be a list of globs");
  ExpectRejected(R"({"tests": [""]})", "non-empty globs");
  ExpectRejected(R"({"tests": [")" + std::string(257, 'a') + R"("]})",
                 "at most 256 characters");
  // The limit counts characters, not UTF-8 bytes, as Python's len() does.
  std::string accented;
  for (int index = 0; index < 256; ++index) {
    accented += "\u00e9";
  }
  static_cast<void>(Rules::Parse(R"({"tests": [")" + accented + R"("]})"));
  ExpectRejected(R"({"tests": [")" + accented + R"(e"]})",
                 "at most 256 characters");
  ExpectRejected(R"({"exclude": ["/rooted"]})", "relative to the repository");
  ExpectRejected(R"({"paths": [{"pattern": "a/**"}]})", "{pattern, language}");
  ExpectRejected(R"({"paths": [{"pattern": "a/**", "language": "zig"}]})",
                 "names an unsupported language");
  for (const std::string_view paths :
       {R"({})", R"("cpp/**")", R"([["cpp/**", "cpp"]])",
        R"([{"language": "cpp"}])",
        R"([{"pattern": "cpp/**", "language": "cpp", "extra": 1}])",
        R"([{"pattern": "", "language": "cpp"}])",
        R"([{"pattern": "cpp/**", "language": null}])",
        R"([{"pattern": 7, "language": "cpp"}])"}) {
    try {
      static_cast<void>(
          Rules::Parse(R"({"paths": )" + std::string(paths) + "}"));
      Expect(false, "malformed path rules are rejected: " + std::string(paths));
    } catch (const llmcc::RulesError&) {  // NOLINT(bugprone-empty-catch)
    }
  }
  std::string many_paths;
  for (int index = 0; index < 500; ++index) {
    many_paths += std::string(index == 0 ? "" : ",") + R"({"pattern": "p)" +
                  std::to_string(index) + R"(", "language": "c"})";
  }
  static_cast<void>(Rules::Parse(R"({"paths": [)" + many_paths + "]}"));
  ExpectRejected(R"({"paths": [)" + many_paths + R"(], "exclude": [)" +
                     Patterns(13) + "]}",
                 "512 glob");
  ExpectRejected(R"({"tests": [)" + Patterns(513) + "]}", "512 glob");
  ExpectRejected(std::string(llmcc::kMaxRulesBytes + 1, ' '), "64 KiB");
  static_cast<void>(Rules::Parse(R"({"tests": [)" + Patterns(512) + "]}"));

  // Worktree rules prefer the current name and fall back to the legacy one.
  const std::filesystem::path temporary(test_tmpdir);
  const auto none = Rules::LoadFromWorktree(temporary / "none");
  Expect(!none.path.has_value() && none.rules.Excluded("third_party/a.cc"),
         "a worktree without rules uses the built-ins");
  Write(temporary / "legacy/.llm-cc/comparison-rules.json",
        R"({"tooling": ["copy.cc"]})");
  const auto legacy = Rules::LoadFromWorktree(temporary / "legacy");
  ExpectEq(legacy.path, std::optional<std::string>(llmcc::kLegacyRulesPath),
           "legacy rules are read");
  ExpectEq(legacy.rules.Classify("copy.cc"), Category::kTooling,
           "legacy rules apply");
  Write(temporary / "legacy/.llm-cc/rules.json", R"({"tests": ["copy.cc"]})");
  const auto current = Rules::LoadFromWorktree(temporary / "legacy");
  ExpectEq(current.path, std::optional<std::string>(llmcc::kRulesPath),
           "the current rules name wins");
  ExpectEq(current.rules.Classify("copy.cc"), Category::kTests,
           "current rules apply");
  Write(temporary / "broken/.llm-cc/rules.json", R"({"tests": 1})");
  try {
    static_cast<void>(Rules::LoadFromWorktree(temporary / "broken"));
    Expect(false, "invalid worktree rules fail");
  } catch (const llmcc::RulesError& error) {
    Expect(std::string(error.what()).starts_with(".llm-cc/rules.json: "),
           "worktree rule errors name the file");
  }
#ifndef _WIN32
  // Unreadable rules are an error, never a silent fallback to other rules.
  if (geteuid() != 0) {
    Write(temporary / "unreadable/.llm-cc/rules.json", R"({"tests": []})");
    std::filesystem::permissions(temporary / "unreadable/.llm-cc",
                                 std::filesystem::perms::none);
    try {
      static_cast<void>(Rules::LoadFromWorktree(temporary / "unreadable"));
      Expect(false, "unreadable worktree rules fail");
    } catch (const llmcc::RulesError& error) {
      Expect(
          std::string(error.what()).find("cannot inspect") != std::string::npos,
          "unreadable worktree rules name the problem");
    }
    std::filesystem::permissions(temporary / "unreadable/.llm-cc",
                                 std::filesystem::perms::owner_all);
  }
  // A dangling or empty .llm-cc link is refused, not read as no rules.
  const std::filesystem::path dangling = temporary / "dangling";
  std::filesystem::create_directories(dangling);
  std::filesystem::create_symlink(temporary / "absent", dangling / ".llm-cc");
  try {
    static_cast<void>(Rules::LoadFromWorktree(dangling));
    Expect(false, "a dangling .llm-cc link fails");
  } catch (const llmcc::RulesError& error) {
    Expect(std::string(error.what()).find("symlink") != std::string::npos,
           "a dangling .llm-cc link is named as a symlink");
  }
  // Rules must be the committed file itself, never a link out of the tree.
  Write(temporary / "outside.json", R"({"tests": ["copy.cc"]})");
  for (const auto& [name, link, target] :
       {std::tuple{"file", ".llm-cc/rules.json", temporary / "outside.json"},
        std::tuple{"directory", ".llm-cc", temporary / "legacy/.llm-cc"}}) {
    const std::filesystem::path repository = temporary / "linked" / name;
    std::filesystem::create_directories((repository / link).parent_path());
    std::filesystem::create_symlink(target, repository / link);
    try {
      static_cast<void>(Rules::LoadFromWorktree(repository));
      Expect(false, std::string("a symlinked rules ") + name + " fails");
    } catch (const llmcc::RulesError& error) {
      Expect(std::string(error.what()).find("symlink") != std::string::npos,
             std::string("a symlinked rules ") + name + " is refused");
    }
  }
#endif

  Expect(llmcc::AlwaysExcluded(".git/config") &&
             llmcc::AlwaysExcluded("a/.llm-cc-cache/b.rs") &&
             !llmcc::AlwaysExcluded("a/.gitignore"),
         "Git metadata and llm-cc caches are fixed exclusions");
  ExpectEq(llmcc::PathExtension("dir.x/Foo.PY"), std::string(".py"),
           "extensions come from the final segment, lowercased");
  ExpectEq(llmcc::PathExtension("dir.x/Makefile"), std::string(),
           "extensionless names have no extension");
  ExpectEq(llmcc::CategoryName(Category::kTooling), std::string_view("tooling"),
           "categories have canonical names");
  return 0;
}
