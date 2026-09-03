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
  llmcc::test::ExpectEq(normal.sources.size(), std::size_t{21},
                        "Git discovery filters headers and generated files");
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
  llmcc::test::ExpectEq(headers.sources.size(), std::size_t{17},
                        "overlap is deduplicated and headers can be included");

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

  const auto all = llmcc::DiscoverSources(
      {repository}, {.include_headers = true, .no_ignore = true});
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
  return 0;
}
