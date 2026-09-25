#include "src/project.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "src/test_util.h"

namespace {

std::string Quote(const std::filesystem::path& path) {
#if defined(_WIN32)
  std::string result = "\"";
  for (char character : path.string()) {
    result += character == '%' ? "^%" : std::string(1, character);
  }
  return result + "\"";
#else
  std::string result = "'";
  for (char character : path.string()) {
    result += character == '\'' ? "'\\''" : std::string(1, character);
  }
  return result + "'";
#endif
}

void Write(const std::filesystem::path& path, std::string_view value = {}) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path) << value;
}

void Run(const std::string& command) {
  llmcc::test::ExpectEq(
      std::system(command.c_str()),  // NOLINT(bugprone-command-processor)
      0,
      "command succeeds");  // NOLINT(bugprone-command-processor)
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  namespace fs = std::filesystem;
  const char* temporary = std::getenv("TEST_TMPDIR");
  llmcc::test::Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const fs::path repository = fs::path(temporary) / "project";
  fs::create_directories(repository);
  Run("git -C " + Quote(repository) + " init -q");
  Write(repository / ".gitignore", "ignored/\nnested/*.cc\n");
  Write(repository / "src/a.rs", "fn a() {}\n");
  Write(repository / "src/bin/server.rs", "fn main() {}\n");
  Write(repository / "src/b.c", "int b;\n");
  Write(repository / "src/z.h", "int z;\n");
  Write(repository / "src/kernel.cu", "__global__ void kernel() {}\n");
  Write(repository / "src/kernel.cuh", "void launch();\n");
  Write(repository / "src/Main.java", "class Main {}\n");
  Write(repository /
            "src/main/java/com/example/adapter/out/persistence/Repository.java",
        "class Repository {}\n");
  Write(repository / "src/tool.py", "def tool(): pass\n");
  Write(repository / "src/window.pyw", "def window(): pass\n");
  Write(repository / "src/types.pyi", "def typed() -> None: ...\n");
  Write(repository / "src/main.go", "package main\n");
  Write(repository / "src/main.js", "function main() {}\n");
  Write(repository / "src/module.mjs", "export function module() {}\n");
  Write(repository / "src/common.cjs", "exports.value = 1;\n");
  Write(repository / "src/Main.cs", "class Main {}\n");
  Write(repository / "src/script.csx", "void Run() {}\n");
  Write(repository / "packages/server/src/index.js",
        "export function start() {}\n");
  Write(repository / "Lib/venv/__init__.py", "def create(): pass\n");
  Write(repository / "internal/env/config.go", "package env\n");
  Write(repository / "ignored/no.cc", "int no;\n");
  Write(repository / "nested/no.cc", "int no;\n");
  Write(repository / "nested/yes.cpp", "int yes;\n");
  Write(repository / "target/generated.rs", "fn generated() {}\n");
  Write(repository / ".venv/hidden.py", "def hidden(): pass\n");
  Write(repository / "venv/pyvenv.cfg", "home = /usr/bin\n");
  Write(repository / "venv/lib/hidden.py", "def hidden(): pass\n");
  Write(repository / "project-env/pyvenv.cfg", "home = /usr/bin\n");
  Write(repository / "project-env/lib/hidden.py", "def hidden(): pass\n");
  Write(repository / "node_modules/hidden.js", "function hidden() {}\n");
  Write(repository / "bin/Hidden.cs", "class Hidden {}\n");
  Write(repository / "bin/tool.csx", "void Run() {}\n");
  Write(repository / "obj/Hidden.cs", "class Hidden {}\n");
  Write(repository / ".gradle/Hidden.java", "class Hidden {}\n");
  Write(repository / "out/Hidden.java", "class Hidden {}\n");
  Write(repository / ".llm-cc-cache/never.rs", "fn never() {}\n");
  const fs::path nested_repository = repository / "nested-repository";
  fs::create_directories(nested_repository);
  Run("git -C " + Quote(nested_repository) + " init -q");
  Write(nested_repository / "nested.rs", "fn nested() {}\n");
  Run("git -C " + Quote(nested_repository) + " add nested.rs");
  Run("git -C " + Quote(repository) +
      " add .gitignore bin/tool.csx internal Lib nested/yes.cpp packages src "
      "target/generated.rs");

  const auto normal = llmcc::DiscoverSources({repository});
  llmcc::test::ExpectEq(normal.sources.size(), std::size_t{23},
                        "Git discovery keeps headers and filters generated "
                        "files");
  llmcc::test::Expect(
      std::ranges::any_of(normal.sources,
                          [](const auto& source) {
                            return source.path.generic_string().ends_with(
                                "bin/tool.csx");
                          }),
      "tracked C# script under bin is preserved");
  llmcc::test::Expect(
      std::ranges::any_of(normal.sources,
                          [](const auto& source) {
                            return source.path.generic_string().ends_with(
                                "Lib/venv/__init__.py");
                          }),
      "tracked Python package named venv is preserved");
  llmcc::test::Expect(
      std::ranges::none_of(normal.sources,
                           [](const auto& source) {
                             const std::string path =
                                 source.path.generic_string();
                             return path.ends_with("venv/lib/hidden.py") ||
                                    path.ends_with("project-env/lib/hidden.py");
                           }),
      "marked Python virtual environments are excluded regardless of name");
  llmcc::test::Expect(
      std::ranges::is_sorted(
          normal.sources, {},
          [](const auto& source) { return source.path.generic_string(); }),
      "sources are sorted");
  const std::map<std::string, llmcc::Language> expected_languages = {
      {"b.c", llmcc::Language::kC},
      {"Main.java", llmcc::Language::kJava},
      {"tool.py", llmcc::Language::kPython},
      {"window.pyw", llmcc::Language::kPython},
      {"types.pyi", llmcc::Language::kPython},
      {"main.go", llmcc::Language::kGo},
      {"main.js", llmcc::Language::kJavaScript},
      {"module.mjs", llmcc::Language::kJavaScript},
      {"common.cjs", llmcc::Language::kJavaScript},
      {"Main.cs", llmcc::Language::kCSharp},
      {"script.csx", llmcc::Language::kCSharp},
      {"kernel.cu", llmcc::Language::kCpp},
      {"z.h", llmcc::Language::kC},
      {"kernel.cuh", llmcc::Language::kCpp},
  };
  for (const auto& [filename, language] : expected_languages) {
    const auto source = std::ranges::find_if(
        normal.sources,
        [&](const auto& item) { return item.path.filename() == filename; });
    llmcc::test::Expect(
        source != normal.sources.end() && source->language == language,
        "new source extension has canonical language");
  }

  const auto headers =
      llmcc::DiscoverSources({repository / "src", repository / "src/a.rs"});
  llmcc::test::ExpectEq(headers.sources.size(), std::size_t{17},
                        "overlap is deduplicated and headers are included");

  const auto explicit_header = llmcc::DiscoverSources({repository / "src/z.h"});
  llmcc::test::ExpectEq(explicit_header.sources.size(), std::size_t{1},
                        "explicit header accepted");
  const auto explicit_cuda_header =
      llmcc::DiscoverSources({repository / "src/kernel.cuh"});
  llmcc::test::ExpectEq(explicit_cuda_header.sources[0].language,
                        llmcc::Language::kCpp,
                        "explicit CUDA header uses C++ grammar");

  const auto forced = llmcc::DiscoverSources(
      {repository / "src/a.rs"}, {.language = llmcc::Language::kCpp});
  llmcc::test::ExpectEq(forced.sources[0].language, llmcc::Language::kCpp,
                        "language is forced");
  const fs::path mixed = fs::path(temporary) / "mixed";
  Write(mixed / "notes.md", "# notes\n");
  Write(mixed / "code.c", "int code;\n");
  const auto forced_directory =
      llmcc::DiscoverSources({mixed}, {.language = llmcc::Language::kCpp});
  llmcc::test::Expect(
      forced_directory.sources.size() == 1 &&
          forced_directory.sources[0].language == llmcc::Language::kCpp,
      "a forced language does not select unsupported files");
  const auto forced_file = llmcc::DiscoverSources(
      {mixed / "notes.md"}, {.language = llmcc::Language::kCpp});
  llmcc::test::ExpectEq(forced_file.sources.size(), std::size_t{1},
                        "an explicit file takes the forced language");

  const auto all = llmcc::DiscoverSources({repository}, {.no_ignore = true});
  llmcc::test::ExpectEq(all.sources.size(), std::size_t{34},
                        "no-ignore includes ignored and generated files");
  for (const auto& source : all.sources) {
    llmcc::test::Expect(
        source.path.string().find(".llm-cc-cache") == std::string::npos,
        "cache directory is permanently excluded");
  }
  const auto nested_source = std::ranges::find_if(
      normal.sources,
      [](const auto& source) { return source.path.filename() == "nested.rs"; });
  llmcc::test::Expect(
      nested_source != normal.sources.end() &&
          nested_source->repository == fs::canonical(nested_repository),
      "nested Git source uses its own cache repository");

  const auto explicit_generated =
      llmcc::DiscoverSources({repository / ".venv/hidden.py"});
  llmcc::test::ExpectEq(explicit_generated.sources.size(), std::size_t{1},
                        "explicit dependency-tree source is accepted");

  const fs::path plain = fs::path(temporary) / "plain";
  Write(plain / "main.c", "int main(void) {}\n");
  Write(plain / ".git", "gitdir: missing\n");
  const auto fallback = llmcc::DiscoverSources({plain});
  llmcc::test::ExpectEq(fallback.sources.size(), std::size_t{1},
                        "filesystem fallback discovers source");
  llmcc::test::Expect(!fallback.warnings.empty(), "fallback emits warning");
  llmcc::test::Expect(!fallback.sources[0].repository.has_value(),
                      "non-Git source has no cache repository");

  const fs::path environment = fs::path(temporary) / "custom-environment";
  Write(environment / "pyvenv.cfg", "home = /usr/bin\n");
  Write(environment / "lib/site-packages/dependency.py",
        "def dependency(): pass\n");
  const auto excluded_environment = llmcc::DiscoverSources({environment});
  llmcc::test::ExpectEq(excluded_environment.sources.size(), std::size_t{0},
                        "virtual environment input root is excluded");
  const auto included_environment =
      llmcc::DiscoverSources({environment}, {.no_ignore = true});
  llmcc::test::ExpectEq(included_environment.sources.size(), std::size_t{1},
                        "no-ignore includes a virtual environment input root");

#ifndef _WIN32
  // Recursive discovery skips symlinks, which would otherwise lend their own
  // path, language and rules to the file they point at.
  for (const bool git : {true, false}) {
    const fs::path aliased =
        fs::path(temporary) / (git ? "aliased-git" : "aliased");
    Write(aliased / "src/main.rs", "fn main() {}\n");
    fs::create_symlink("src/main.rs", aliased / "a.py");
    if (git) {
      Run("git -C " + Quote(aliased) + " init -q");
    }
    const auto discovered = llmcc::DiscoverSources({aliased});
    llmcc::test::Expect(
        discovered.sources.size() == 1 &&
            discovered.sources[0].language == llmcc::Language::kRust &&
            discovered.sources[0].relative_path == "src/main.rs",
        "a symlink alias does not classify its target");
  }
#endif

  // A repository nested in a plain directory keeps its own rules.
  const fs::path outer = fs::path(temporary) / "outer";
  const fs::path inner = outer / "inner";
  fs::create_directories(inner);
  Run("git -C " + Quote(inner) + " init -q");
  Write(inner / ".llm-cc/rules.json", R"({"exclude": ["gen/**"]})");
  Write(inner / "gen/skip.cc", "int skip;\n");
  Write(inner / "keep.cc", "int keep;\n");
  Write(outer / "top.cc", "int top;\n");
  const auto nested_rules = llmcc::DiscoverSources({outer});
  std::map<std::string, const llmcc::DiscoveredSource*> nested_by_path;
  for (const auto& source : nested_rules.sources) {
    nested_by_path[source.relative_path] = &source;
  }
  llmcc::test::Expect(
      nested_by_path.size() == 2 && nested_by_path.contains("top.cc") &&
          nested_by_path.contains("keep.cc") &&
          nested_by_path.at("keep.cc")->repository == fs::canonical(inner),
      "a nested repository is discovered with its own rules");

  // A repository's rules select, name, and classify its sources.
  const fs::path ruled = fs::path(temporary) / "ruled";
  fs::create_directories(ruled);
  Run("git -C " + Quote(ruled) + " init -q");
  Write(ruled / ".llm-cc/rules.json",
        R"({"exclude": ["gen/**", "**/*.pb.cc"], "tests": ["qa/**"],
            "extensions": {".h": "cpp"},
            "paths": [{"pattern": "legacy/**", "language": "c"}]})");
  Write(ruled / "src/Upper.H", "int upper;\n");
  Write(ruled / "src/Other.CPP", "int other;\n");
  Write(ruled / "src/main.cc", "int main() {}\n");
  Write(ruled / "src/msg.pb.cc", "int generated;\n");
  Write(ruled / "legacy/x.h", "int legacy;\n");
  Write(ruled / "gen/skip.cc", "int skip;\n");
  Write(ruled / "qa/check.py", "def check(): pass\n");
  Write(ruled / "third_party/kept.cc", "int kept;\n");
  const fs::path ruled_nested = ruled / "nested";
  fs::create_directories(ruled_nested);
  Run("git -C " + Quote(ruled_nested) + " init -q");
  Write(ruled_nested / "gen/kept.cc", "int kept;\n");
  Write(ruled_nested / "third_party/skip.cc", "int skip;\n");
  const auto ruled_sources = llmcc::DiscoverSources({ruled});
  std::map<std::string, const llmcc::DiscoveredSource*> by_relative;
  for (const auto& source : ruled_sources.sources) {
    const std::string key =
        (source.repository == fs::canonical(ruled_nested) ? "nested:" : "") +
        source.relative_path;
    by_relative[key] = &source;
  }
  llmcc::test::ExpectEq(ruled_sources.sources.size(), std::size_t{7},
                        "repository rules select sources");
  const auto language_of = [&](const std::string& key) {
    const auto found = by_relative.find(key);
    llmcc::test::Expect(found != by_relative.end(), "source found: " + key);
    return found->second->language;
  };
  llmcc::test::ExpectEq(language_of("src/Upper.H"), llmcc::Language::kCpp,
                        "configured extensions match case-insensitively");
  llmcc::test::ExpectEq(language_of("src/Other.CPP"), llmcc::Language::kCpp,
                        "built-in extensions match case-insensitively");
  llmcc::test::ExpectEq(language_of("legacy/x.h"), llmcc::Language::kC,
                        "path overrides select a language");
  llmcc::test::ExpectEq(language_of("third_party/kept.cc"),
                        llmcc::Language::kCpp,
                        "a configured exclude replaces the defaults");
  llmcc::test::ExpectEq(language_of("nested:gen/kept.cc"),
                        llmcc::Language::kCpp,
                        "a nested repository uses its own rules");
  llmcc::test::ExpectEq(by_relative.at("qa/check.py")->category,
                        llmcc::Category::kTests,
                        "configured test globs classify sources");
  llmcc::test::ExpectEq(by_relative.at("src/main.cc")->category,
                        llmcc::Category::kRuntime,
                        "unmatched sources are runtime");
  llmcc::test::ExpectEq(ruled_sources.rules.size(), std::size_t{2},
                        "each Git root reports its rules");
  llmcc::test::Expect(
      ruled_sources.rules[0].repository == fs::canonical(ruled) &&
          ruled_sources.rules[0].path == std::string(llmcc::kRulesPath) &&
          ruled_sources.rules[1].repository == fs::canonical(ruled_nested) &&
          !ruled_sources.rules[1].path.has_value(),
      "rules sources name the file each root used");
  const auto ruled_all = llmcc::DiscoverSources({ruled}, {.no_ignore = true});
  llmcc::test::ExpectEq(ruled_all.sources.size(), std::size_t{10},
                        "no-ignore includes rule-excluded sources");
  const auto ruled_explicit = llmcc::DiscoverSources({ruled / "gen/skip.cc"});
  llmcc::test::Expect(
      ruled_explicit.sources.size() == 1 &&
          ruled_explicit.sources[0].relative_path == "gen/skip.cc",
      "an explicit excluded file is still analyzed");
  Write(ruled / ".llm-cc/rules.json", R"({"tests": 1})");
  try {
    static_cast<void>(llmcc::DiscoverSources({ruled}));
    llmcc::test::Expect(false, "invalid repository rules fail discovery");
  } catch (const std::runtime_error& error) {
    llmcc::test::Expect(
        std::string(error.what()).find("invalid rules in") != std::string::npos,
        "invalid rules name their repository");
  }
  return 0;
}
