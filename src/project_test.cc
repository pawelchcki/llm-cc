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
  std::string result = "'";
  for (char character : path.string()) {
    result += character == '\'' ? "'\\''" : std::string(1, character);
  }
  return result + "'";
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
  Write(repository / "src/b.c", "int b;\n");
  Write(repository / "src/z.h", "int z;\n");
  Write(repository / "src/Main.java", "class Main {}\n");
  Write(repository / "src/tool.py", "def tool(): pass\n");
  Write(repository / "src/window.pyw", "def window(): pass\n");
  Write(repository / "src/types.pyi", "def typed() -> None: ...\n");
  Write(repository / "src/main.go", "package main\n");
  Write(repository / "src/main.js", "function main() {}\n");
  Write(repository / "src/module.mjs", "export function module() {}\n");
  Write(repository / "src/common.cjs", "exports.value = 1;\n");
  Write(repository / "src/Main.cs", "class Main {}\n");
  Write(repository / "src/script.csx", "void Run() {}\n");
  Write(repository / "ignored/no.cc", "int no;\n");
  Write(repository / "nested/no.cc", "int no;\n");
  Write(repository / "nested/yes.cpp", "int yes;\n");
  Write(repository / "target/generated.rs", "fn generated() {}\n");
  Write(repository / ".venv/hidden.py", "def hidden(): pass\n");
  Write(repository / "node_modules/hidden.js", "function hidden() {}\n");
  Write(repository / "bin/Hidden.cs", "class Hidden {}\n");
  Write(repository / "obj/Hidden.cs", "class Hidden {}\n");
  Write(repository / ".gradle/Hidden.java", "class Hidden {}\n");
  Write(repository / ".llm-cc-cache/never.rs", "fn never() {}\n");
  const fs::path nested_repository = repository / "nested-repository";
  fs::create_directories(nested_repository);
  Run("git -C " + Quote(nested_repository) + " init -q");
  Write(nested_repository / "nested.rs", "fn nested() {}\n");
  Run("git -C " + Quote(nested_repository) + " add nested.rs");
  Run("git -C " + Quote(repository) +
      " add .gitignore src nested/yes.cpp target/generated.rs");

  const auto normal = llmcc::DiscoverSources({repository});
  llmcc::test::ExpectEq(normal.sources.size(), std::size_t{14},
                        "Git discovery filters headers and generated files");
  llmcc::test::Expect(
      std::ranges::is_sorted(
          normal.sources, {},
          [](const auto& source) { return source.path.generic_string(); }),
      "sources are sorted");
  const std::map<std::string, llmcc::Language> expected_languages = {
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
  };
  for (const auto& [filename, language] : expected_languages) {
    const auto source = std::ranges::find_if(
        normal.sources,
        [&](const auto& item) { return item.path.filename() == filename; });
    llmcc::test::Expect(
        source != normal.sources.end() && source->language == language,
        "new source extension has canonical language");
  }

  const auto headers = llmcc::DiscoverSources(
      {repository / "src", repository / "src/a.rs"}, {.include_headers = true});
  llmcc::test::ExpectEq(headers.sources.size(), std::size_t{13},
                        "overlap is deduplicated and headers can be included");

  const auto explicit_header = llmcc::DiscoverSources({repository / "src/z.h"});
  llmcc::test::ExpectEq(explicit_header.sources.size(), std::size_t{1},
                        "explicit header accepted");

  const auto forced = llmcc::DiscoverSources(
      {repository / "src/a.rs"}, {.language = llmcc::Language::kCpp});
  llmcc::test::ExpectEq(forced.sources[0].language, llmcc::Language::kCpp,
                        "language is forced");

  const auto all = llmcc::DiscoverSources(
      {repository}, {.include_headers = true, .no_ignore = true});
  llmcc::test::ExpectEq(all.sources.size(), std::size_t{23},
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
  const auto fallback = llmcc::DiscoverSources({plain});
  llmcc::test::ExpectEq(fallback.sources.size(), std::size_t{1},
                        "filesystem fallback discovers source");
  llmcc::test::Expect(!fallback.warnings.empty(), "fallback emits warning");
  llmcc::test::Expect(!fallback.sources[0].repository.has_value(),
                      "non-Git source has no cache repository");
  return 0;
}
