#include "src/lang.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/test_util.h"

namespace {

std::string Read(std::string_view path) {
  std::ifstream input(std::string(path), std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void CheckComments(std::string_view path, llmcc::Language language,
                   bool check_literals = false) {
  const std::string source = Read(path);
  const auto [stripped, map] = llmcc::StripComments(source, language);
  llmcc::test::Expect(stripped.find("block comment") == std::string::npos,
                      "block comment removed");
  if (source.find("html comment") != std::string::npos) {
    llmcc::test::Expect(stripped.find("html comment") == std::string::npos,
                        "JavaScript HTML comment removed");
  }
  if (source.starts_with("#!")) {
    llmcc::test::Expect(stripped.find("#!/usr/bin/env") == std::string::npos,
                        "JavaScript hash-bang line removed");
  }
  llmcc::test::ExpectEq(map.size(), stripped.size() + 1,
                        "offset map has every boundary");
  llmcc::test::ExpectEq(map.back(), source.size(), "offset map ends at source");
  llmcc::test::ExpectEq(std::ranges::count(stripped, '\n'),
                        std::ranges::count(source, '\n'),
                        "comment removal preserves newlines");
  llmcc::test::Expect(std::ranges::is_sorted(map), "offset map is monotonic");
  if (check_literals) {
    llmcc::test::Expect(stripped.find("not a comment") != std::string::npos &&
                            stripped.find("still text") != std::string::npos,
                        "comment markers inside literals are preserved");
  }
}

void CheckStructure(std::string_view path, llmcc::Language language,
                    std::size_t minimum_events, std::size_t minimum_depth) {
  const auto events = llmcc::StructuralEvents(Read(path), language);
  llmcc::test::Expect(events.size() >= minimum_events,
                      std::string(path) + " emitted " +
                          std::to_string(events.size()) + " structural events");
  llmcc::test::Expect(std::ranges::any_of(events,
                                          [minimum_depth](const auto& event) {
                                            return event.depth >= minimum_depth;
                                          }),
                      std::string(path) + " preserves nested structural depth");
}

void CheckFunctionNames(std::string_view path, llmcc::Language language,
                        const std::vector<std::string>& expected) {
  const std::string source = Read(path);
  const auto functions = llmcc::Functions(source, language);
  std::vector<std::string> names;
  for (const auto& function : functions) {
    names.push_back(function.name);
    llmcc::test::Expect(function.start_byte < function.end_byte &&
                            function.end_byte <= source.size(),
                        "callable span is within the source");
  }
  llmcc::test::ExpectEq(names, expected,
                        "only outermost named callables are reported");
}

void CheckFunctionSpan(std::string_view path, llmcc::Language language,
                       std::size_t index, std::string_view expected) {
  const std::string source = Read(path);
  const auto functions = llmcc::Functions(source, language);
  llmcc::test::Expect(index < functions.size(), "callable span exists");
  const auto& function = functions[index];
  llmcc::test::ExpectEq(source.substr(function.start_byte,
                                      function.end_byte - function.start_byte),
                        std::string(expected),
                        "callable span matches its exact declaration");
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  CheckComments("testdata/lang/comments.rs", llmcc::Language::kRust);
  CheckComments("testdata/lang/comments.c", llmcc::Language::kC);
  CheckComments("testdata/lang/comments.cc", llmcc::Language::kCpp);
  CheckComments("testdata/lang/comments.java", llmcc::Language::kJava, true);
  CheckComments("testdata/lang/comments.py", llmcc::Language::kPython, true);
  CheckComments("testdata/lang/comments.go", llmcc::Language::kGo, true);
  CheckComments("testdata/lang/comments.js", llmcc::Language::kJavaScript,
                true);
  CheckComments("testdata/lang/comments.cs", llmcc::Language::kCSharp, true);

  const std::string rust = Read("testdata/lang/structure.rs");
  const auto rust_events =
      llmcc::StructuralEvents(rust, llmcc::Language::kRust);
  llmcc::test::Expect(rust_events.size() >= 14, "Rust structural events");
  llmcc::test::Expect(
      std::ranges::any_of(rust_events,
                          [](const auto& event) { return event.depth >= 4; }),
      "Rust nested depth");

  const std::string c = Read("testdata/lang/structure.c");
  llmcc::test::Expect(
      llmcc::StructuralEvents(c, llmcc::Language::kC).size() >= 12,
      "C structural events");
  const std::string cpp = Read("testdata/lang/structure.cc");
  llmcc::test::Expect(
      llmcc::StructuralEvents(cpp, llmcc::Language::kCpp).size() >= 16,
      "C++ structural events");
  CheckStructure("testdata/lang/structure.java", llmcc::Language::kJava, 18, 6);
  const auto guarded_switch = llmcc::StructuralEvents(
      "class G { void m(Object v) { switch (v) { case String s when "
      "!s.isEmpty() -> {} } } }",
      llmcc::Language::kJava);
  const auto unguarded_switch = llmcc::StructuralEvents(
      "class G { void m(Object v) { switch (v) { case String s -> {} } } }",
      llmcc::Language::kJava);
  llmcc::test::ExpectEq(guarded_switch.size(), unguarded_switch.size() + 1,
                        "Java switch guard is structural");
  CheckStructure("testdata/lang/structure.py", llmcc::Language::kPython, 18, 6);
  const auto comprehension_events = llmcc::StructuralEvents(
      "[f(x) for xs in groups for x in xs if x]", llmcc::Language::kPython);
  llmcc::test::ExpectEq(comprehension_events.size(), std::size_t{3},
                        "Python comprehension loops and branch are structural");
  std::vector<std::size_t> comprehension_depths;
  std::ranges::transform(comprehension_events,
                         std::back_inserter(comprehension_depths),
                         [](const auto& event) { return event.depth; });
  llmcc::test::ExpectEq(comprehension_depths,
                        std::vector<std::size_t>({0, 1, 2}),
                        "Python comprehension clauses preserve nesting");
  const auto exception_group_events =
      llmcc::StructuralEvents("try:\n    pass\nexcept* ValueError:\n    pass\n",
                              llmcc::Language::kPython);
  llmcc::test::ExpectEq(exception_group_events.size(), std::size_t{4},
                        "Python exception-group handler is structural");
  CheckStructure("testdata/lang/structure.go", llmcc::Language::kGo, 12, 5);
  const auto go_default_events = llmcc::StructuralEvents(
      "package p\nfunc run() { switch { default: return } }",
      llmcc::Language::kGo);
  llmcc::test::ExpectEq(go_default_events.size(), std::size_t{4},
                        "Go default case is structural");
  CheckStructure("testdata/lang/structure.js", llmcc::Language::kJavaScript, 18,
                 6);
  CheckStructure("testdata/lang/structure.cs", llmcc::Language::kCSharp, 18, 6);
  const auto csharp_query_events = llmcc::StructuralEvents(
      "class Q { void M() { var q = from x in xs where x > 0 select x; } }",
      llmcc::Language::kCSharp);
  llmcc::test::ExpectEq(csharp_query_events.size(), std::size_t{6},
                        "C# query clauses are structural");
  const auto filtered_catch = llmcc::StructuralEvents(
      "class Q { void M() { try {} catch (Exception e) when (P(e)) {} } }",
      llmcc::Language::kCSharp);
  const auto unfiltered_catch = llmcc::StructuralEvents(
      "class Q { void M() { try {} catch (Exception e) {} } }",
      llmcc::Language::kCSharp);
  llmcc::test::ExpectEq(filtered_catch.size(), unfiltered_catch.size() + 1,
                        "C# catch filter is structural");
  const auto structure_functions = llmcc::Functions(cpp, llmcc::Language::kCpp);
  llmcc::test::ExpectEq(structure_functions.size(), std::size_t{1},
                        "one C++ function, excluding lambda");
  llmcc::test::ExpectEq(structure_functions.front().name, std::string("run"),
                        "in-class member name");
  llmcc::test::ExpectEq(
      cpp.substr(structure_functions.front().start_byte,
                 structure_functions.front().end_byte -
                     structure_functions.front().start_byte),
      std::string_view("int run(int value) {\n"
                       "        auto adjust = [value](int step) {\n"
                       "            for (int item : {1, 2}) {\n"
                       "                if (item > 0) {\n"
                       "                    switch (item) {\n"
                       "                        case 1: value += step; break;\n"
                       "                        default: break;\n"
                       "                    }\n"
                       "                }\n"
                       "            }\n"
                       "            return value;\n"
                       "        };\n"
                       "        try {\n"
                       "            return adjust(1);\n"
                       "        } catch (...) {\n"
                       "            return 0;\n"
                       "        }\n"
                       "    }"),
      "C++ function span");

  const std::string cpp_functions = Read("testdata/lang/functions.cc");
  const auto extracted_cpp =
      llmcc::Functions(cpp_functions, llmcc::Language::kCpp);
  std::vector<std::string> cpp_names;
  for (const auto& function : extracted_cpp) {
    cpp_names.push_back(function.name);
  }
  llmcc::test::ExpectEq(
      cpp_names,
      std::vector<std::string>({"pointer_result", "reference_result",
                                "Ns::Cls::~Cls", "operator<<",
                                "operator bool() const"}),
      "complex C++ function names");

  const std::string rust_functions = Read("testdata/lang/functions.rs");
  const auto extracted_rust =
      llmcc::Functions(rust_functions, llmcc::Language::kRust);
  std::vector<std::string> rust_names;
  for (const auto& function : extracted_rust) {
    rust_names.push_back(function.name);
  }
  llmcc::test::ExpectEq(
      rust_names, std::vector<std::string>({"method", "outer"}),
      "Rust impl method included and nested function excluded");

  CheckFunctionNames("testdata/lang/functions.java", llmcc::Language::kJava,
                     {"Point", "Widget", "compute"});
  CheckFunctionSpan("testdata/lang/functions.java", llmcc::Language::kJava, 0,
                    "Point {\n        if (x < 0) throw new "
                    "IllegalArgumentException();\n    }");
  CheckFunctionNames("testdata/lang/functions.py", llmcc::Language::kPython,
                     {"decorated", "method"});
  const auto python_functions = llmcc::Functions(
      Read("testdata/lang/functions.py"), llmcc::Language::kPython);
  llmcc::test::ExpectEq(
      python_functions.front().start_byte, std::size_t{0},
      "decorated Python function span starts at its first decorator");
  CheckFunctionSpan("testdata/lang/functions.py", llmcc::Language::kPython, 0,
                    "@first\n@second(value=1)\ndef decorated(value):\n"
                    "    def hidden():\n        return 0\n    return value");
  CheckFunctionNames("testdata/lang/functions.go", llmcc::Language::kGo,
                     {"top", "Method"});
  CheckFunctionSpan("testdata/lang/functions.go", llmcc::Language::kGo, 1,
                    "func (Widget) Method() int { return 1 }");
  CheckFunctionNames("testdata/lang/functions.js", llmcc::Language::kJavaScript,
                     {"top", "generate", "method", "objectMethod"});
  CheckFunctionSpan("testdata/lang/functions.js", llmcc::Language::kJavaScript,
                    3, "objectMethod() {\n    const hidden = () => {};\n  }");
  CheckFunctionNames(
      "testdata/lang/functions.cs", llmcc::Language::kCSharp,
      {"Number", "~Number", "operator +", "implicit operator int", "Run"});
  CheckFunctionSpan("testdata/lang/functions.cs", llmcc::Language::kCSharp, 3,
                    "public static implicit operator int(Number value) => 0;");

  llmcc::test::ExpectEq(llmcc::LineStarts("one\ntwo\n"),
                        std::vector<std::size_t>({0, 4, 8}), "line starts");
  llmcc::test::ExpectEq(llmcc::LineStarts(""), std::vector<std::size_t>({0}),
                        "empty source has first line");

  llmcc::test::ExpectEq(llmcc::InferLanguage("source.rs"),
                        llmcc::Language::kRust, "infer Rust");
  llmcc::test::ExpectEq(llmcc::ParseLanguage("c++"), llmcc::Language::kCpp,
                        "parse C++ alias");
  const std::vector<std::pair<std::string_view, llmcc::Language>> aliases = {
      {"java", llmcc::Language::kJava},
      {"python", llmcc::Language::kPython},
      {"py", llmcc::Language::kPython},
      {"go", llmcc::Language::kGo},
      {"golang", llmcc::Language::kGo},
      {"javascript", llmcc::Language::kJavaScript},
      {"js", llmcc::Language::kJavaScript},
      {"node", llmcc::Language::kJavaScript},
      {"nodejs", llmcc::Language::kJavaScript},
      {"node.js", llmcc::Language::kJavaScript},
      {"csharp", llmcc::Language::kCSharp},
      {"cs", llmcc::Language::kCSharp},
      {"c#", llmcc::Language::kCSharp},
  };
  for (const auto& [alias, language] : aliases) {
    llmcc::test::ExpectEq(llmcc::ParseLanguage(alias), language,
                          "language alias parses");
    llmcc::test::ExpectEq(llmcc::ParseLanguage(llmcc::LanguageName(language)),
                          language, "canonical language name round trips");
  }
  const std::vector<std::pair<std::string_view, llmcc::Language>> extensions = {
      {"Source.java", llmcc::Language::kJava},
      {"source.py", llmcc::Language::kPython},
      {"source.pyw", llmcc::Language::kPython},
      {"source.pyi", llmcc::Language::kPython},
      {"source.go", llmcc::Language::kGo},
      {"source.js", llmcc::Language::kJavaScript},
      {"source.mjs", llmcc::Language::kJavaScript},
      {"source.cjs", llmcc::Language::kJavaScript},
      {"source.cs", llmcc::Language::kCSharp},
      {"source.csx", llmcc::Language::kCSharp},
  };
  for (const auto& [path, language] : extensions) {
    llmcc::test::ExpectEq(llmcc::InferLanguage(path), language,
                          "source extension is inferred");
  }
  llmcc::test::Expect(!llmcc::IsSourcePath("component.jsx", false) &&
                          !llmcc::IsSourcePath("component.ts", false) &&
                          !llmcc::IsSourcePath("component.tsx", false) &&
                          !llmcc::IsSourcePath("Main.JAVA", false),
                      "unsupported and case-mismatched extensions are omitted");
  try {
    static_cast<void>(llmcc::ParseLanguage("typescript"));
    llmcc::test::Expect(false, "unsupported language is rejected");
  } catch (const std::invalid_argument& error) {
    llmcc::test::Expect(
        std::string(error.what())
                .find(
                    "rust, c, cpp, java, python, go, javascript, or csharp") !=
            std::string::npos,
        "language validation error lists canonical names");
  }
  return 0;
}
