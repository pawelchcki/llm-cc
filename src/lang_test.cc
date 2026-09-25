#include "src/lang.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "src/test_util.h"

namespace {

std::filesystem::path fixture_root;

std::string Read(std::string_view path) {
  std::ifstream input(fixture_root / std::filesystem::path(path).filename(),
                      std::ios::binary);
  llmcc::test::Expect(input.is_open(), "language fixture exists");
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
  for (std::size_t i = 1; i < map.size(); ++i) {
    llmcc::test::Expect(map.at(i - 1) <= map.at(i), "offset map is monotonic");
  }
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

void CheckPreparedParity(std::string_view path, llmcc::Language language) {
  const std::string source = Read(path);
  const llmcc::PreparedSource prepared = llmcc::PrepareSource(source, language);

  auto prepared_events = prepared.structural_events;
  for (auto& event : prepared_events) {
    event.scope_start =
        prepared.original_offsets.OriginalOffset(event.scope_start);
    event.byte_offset =
        prepared.original_offsets.OriginalOffset(event.byte_offset);
  }
  const auto event_order = [](const llmcc::StructuralEvent& event) {
    return std::tuple(event.byte_offset, event.depth, event.scope_start);
  };
  std::ranges::sort(prepared_events, {}, event_order);
  llmcc::test::ExpectEq(prepared_events,
                        llmcc::StructuralEvents(source, language),
                        "combined traversal preserves structural events");

  auto prepared_functions = prepared.functions;
  for (auto& function : prepared_functions) {
    function.start_byte =
        prepared.original_offsets.OriginalOffset(function.start_byte);
    function.end_byte =
        prepared.original_offsets.OriginalOffset(function.end_byte);
  }
  llmcc::test::ExpectEq(prepared_functions, llmcc::Functions(source, language),
                        "combined traversal preserves callable extraction");
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape)
  const char* test_srcdir = std::getenv("TEST_SRCDIR");
  llmcc::test::Expect(argc == 2 && test_srcdir != nullptr,
                      "Bazel supplies the language fixture runfile key");
  fixture_root = (std::filesystem::path(test_srcdir) / argv[1]).parent_path();
  CheckComments("testdata/lang/comments.rs", llmcc::Language::kRust);
  CheckComments("testdata/lang/comments.c", llmcc::Language::kC);
  CheckComments("testdata/lang/comments.cc", llmcc::Language::kCpp);
  CheckComments("testdata/lang/comments.java", llmcc::Language::kJava, true);
  CheckComments("testdata/lang/comments.py", llmcc::Language::kPython, true);
  CheckComments("testdata/lang/comments.go", llmcc::Language::kGo, true);
  CheckComments("testdata/lang/comments.js", llmcc::Language::kJavaScript,
                true);
  CheckComments("testdata/lang/comments.cs", llmcc::Language::kCSharp, true);
  for (const auto& [path, language] :
       std::vector<std::pair<std::string_view, llmcc::Language>>{
           {"testdata/lang/structure.rs", llmcc::Language::kRust},
           {"testdata/lang/structure.c", llmcc::Language::kC},
           {"testdata/lang/structure.cc", llmcc::Language::kCpp},
           {"testdata/lang/structure.java", llmcc::Language::kJava},
           {"testdata/lang/structure.py", llmcc::Language::kPython},
           {"testdata/lang/structure.go", llmcc::Language::kGo},
           {"testdata/lang/structure.js", llmcc::Language::kJavaScript},
           {"testdata/lang/structure.cs", llmcc::Language::kCSharp}}) {
    CheckPreparedParity(path, language);
  }

  const auto [compact_source, compact_map] =
      llmcc::StripComments("a/* removed */b\n", llmcc::Language::kCpp);
  llmcc::test::ExpectEq(compact_source, std::string("a b\n"),
                        "inline comment separation is retained");
  llmcc::test::ExpectEq(compact_map.size(), compact_source.size() + 1,
                        "compact map contains all logical boundaries");
  llmcc::test::Expect(compact_map.span_count() < compact_map.size(),
                      "retained byte runs are compacted");
  llmcc::test::ExpectEq(compact_map.OriginalOffset(compact_source.find('b')),
                        std::string_view("a/* removed */b\n").find('b'),
                        "compact map resolves cleaned offsets");
  llmcc::test::ExpectEq(
      compact_map.CleanedOffset(
          std::string_view("a/* removed */b\n").find("removed")),
      compact_source.find('b'),
      "inverse map preserves dense lower-bound semantics");

  bool timed_out = false;
  try {
    static_cast<void>(
        llmcc::PrepareSource("int f() { return 1; }", llmcc::Language::kCpp,
                             {.time_budget = std::chrono::milliseconds(0)}));
  } catch (const std::runtime_error& error) {
    timed_out = std::string_view(error.what()).find("time budget") !=
                std::string_view::npos;
  }
  llmcc::test::Expect(timed_out,
                      "an injectable preprocessing deadline is enforced");

  std::string wide_timeout_source = "int values[]={";
  constexpr std::size_t kTimeoutFixtureBytes = 1024ULL * 1024ULL;
  wide_timeout_source.reserve(kTimeoutFixtureBytes + 4);
  while (wide_timeout_source.size() < kTimeoutFixtureBytes) {
    wide_timeout_source += "0,";
  }
  wide_timeout_source += "0};\n";
  const auto timeout_start = std::chrono::steady_clock::now();
  bool parser_timed_out = false;
  try {
    static_cast<void>(
        llmcc::PrepareSource(wide_timeout_source, llmcc::Language::kCpp,
                             {.time_budget = std::chrono::milliseconds(10)}));
  } catch (const std::runtime_error& error) {
    parser_timed_out =
        std::string_view(error.what()).find("during tree-sitter parsing") !=
        std::string_view::npos;
  }
  const auto timeout_elapsed = std::chrono::steady_clock::now() - timeout_start;
  llmcc::test::Expect(
      parser_timed_out && timeout_elapsed < std::chrono::seconds(5),
      "tree-sitter cooperatively cancels a large parse within its deadline");

  bool too_deep = false;
  try {
    static_cast<void>(llmcc::PrepareSource("int f() { if (1) { return 1; } }",
                                           llmcc::Language::kCpp,
                                           {.max_syntax_depth = 2}));
  } catch (const std::runtime_error& error) {
    too_deep = std::string_view(error.what()).find("depth limit") !=
               std::string_view::npos;
  }
  llmcc::test::Expect(too_deep,
                      "syntax traversal has an explicit depth budget");

  std::string wide_comprehension = "[x";
  for (std::size_t clause = 0; clause < 65; ++clause) {
    wide_comprehension += " for x" + std::to_string(clause) + " in xs";
  }
  wide_comprehension += "]";
  bool structurally_too_deep = false;
  try {
    static_cast<void>(llmcc::PrepareSource(wide_comprehension,
                                           llmcc::Language::kPython,
                                           {.max_syntax_depth = 64}));
  } catch (const std::runtime_error& error) {
    structurally_too_deep =
        std::string_view(error.what()).find("structural depth") !=
        std::string_view::npos;
  }
  llmcc::test::Expect(
      structurally_too_deep,
      "wide comprehensions cannot create an unsafe output hierarchy depth");

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
  const auto java_conditional = llmcc::StructuralEvents(
      "class T { int f(boolean b) { return b ? 1 : 0; } }",
      llmcc::Language::kJava);
  const auto java_unconditional = llmcc::StructuralEvents(
      "class T { int f(boolean b) { return 1; } }", llmcc::Language::kJava);
  llmcc::test::ExpectEq(java_conditional.size(), java_unconditional.size() + 1,
                        "Java conditional expression is structural");
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
  const std::vector<std::string_view> nested_comprehensions = {
      "[[y for y in x] for x in xs]",
      "{x: [y for y in x] for x in xs}",
      "{[y for y in x] for x in xs}",
      "([y for y in x] for x in xs)",
  };
  for (const std::string_view source : nested_comprehensions) {
    const auto nested_events =
        llmcc::StructuralEvents(source, llmcc::Language::kPython);
    llmcc::test::ExpectEq(nested_events.size(), std::size_t{2},
                          "nested Python comprehension has two loops");
    llmcc::test::ExpectEq(nested_events.front().depth, std::size_t{1},
                          "result comprehension is inside the outer loop");
    llmcc::test::ExpectEq(nested_events.back().depth, std::size_t{0},
                          "outer comprehension loop begins at parent depth");
  }
  const auto nested_first_iterable = llmcc::StructuralEvents(
      "[x for x in [y for y in ys]]", llmcc::Language::kPython);
  std::vector<std::size_t> first_iterable_depths;
  std::ranges::transform(nested_first_iterable,
                         std::back_inserter(first_iterable_depths),
                         [](const auto& event) { return event.depth; });
  llmcc::test::ExpectEq(
      first_iterable_depths, std::vector<std::size_t>({0, 0}),
      "first comprehension iterable precedes its own loop scope");
  const auto nested_later_iterable = llmcc::StructuralEvents(
      "[z for x in xs for z in [y for y in x]]", llmcc::Language::kPython);
  std::vector<std::size_t> later_iterable_depths;
  std::ranges::transform(nested_later_iterable,
                         std::back_inserter(later_iterable_depths),
                         [](const auto& event) { return event.depth; });
  llmcc::test::ExpectEq(
      later_iterable_depths, std::vector<std::size_t>({0, 1, 1}),
      "later comprehension iterable follows only preceding loop scopes");
  const auto nested_filter = llmcc::StructuralEvents(
      "[x for x in xs if any(y for y in ys)]", llmcc::Language::kPython);
  std::vector<std::size_t> filter_depths;
  std::ranges::transform(nested_filter, std::back_inserter(filter_depths),
                         [](const auto& event) { return event.depth; });
  llmcc::test::ExpectEq(
      filter_depths, std::vector<std::size_t>({0, 1, 1}),
      "comprehension filter predicate precedes its own decision scope");
  const auto exception_group_events =
      llmcc::StructuralEvents("try:\n    pass\nexcept* ValueError:\n    pass\n",
                              llmcc::Language::kPython);
  llmcc::test::ExpectEq(exception_group_events.size(), std::size_t{4},
                        "Python exception-group handler is structural");
  const auto python_conditional = llmcc::StructuralEvents(
      "def f(flag):\n    return 1 if flag else 0\n", llmcc::Language::kPython);
  const auto python_unconditional = llmcc::StructuralEvents(
      "def f(flag):\n    return 1\n", llmcc::Language::kPython);
  llmcc::test::ExpectEq(python_conditional.size(),
                        python_unconditional.size() + 1,
                        "Python conditional expression is structural");
  CheckStructure("testdata/lang/structure.go", llmcc::Language::kGo, 12, 5);
  const auto go_default_events = llmcc::StructuralEvents(
      "package p\nfunc run() { switch { default: return } }",
      llmcc::Language::kGo);
  llmcc::test::ExpectEq(go_default_events.size(), std::size_t{4},
                        "Go default case is structural");
  CheckStructure("testdata/lang/structure.js", llmcc::Language::kJavaScript, 18,
                 6);
  const auto javascript_conditional =
      llmcc::StructuralEvents("function f(flag) { return flag ? 1 : 0; }",
                              llmcc::Language::kJavaScript);
  const auto javascript_unconditional = llmcc::StructuralEvents(
      "function f(flag) { return 1; }", llmcc::Language::kJavaScript);
  llmcc::test::ExpectEq(javascript_conditional.size(),
                        javascript_unconditional.size() + 1,
                        "JavaScript conditional expression is structural");
  CheckStructure("testdata/lang/structure.cs", llmcc::Language::kCSharp, 18, 6);
  const auto csharp_query_events = llmcc::StructuralEvents(
      "class Q { void M() { var q = from x in xs where x > 0 orderby x "
      "select x; } }",
      llmcc::Language::kCSharp);
  llmcc::test::ExpectEq(csharp_query_events.size(), std::size_t{7},
                        "C# query clauses are structural");
  const auto csharp_deconstruction_foreach = llmcc::StructuralEvents(
      "class Q { void M() { foreach (var (key, value) in pairs) {} } }",
      llmcc::Language::kCSharp);
  const auto csharp_regular_foreach = llmcc::StructuralEvents(
      "class Q { void M() { foreach (var pair in pairs) {} } }",
      llmcc::Language::kCSharp);
  llmcc::test::ExpectEq(csharp_deconstruction_foreach.size(),
                        csharp_regular_foreach.size(),
                        "C# deconstruction foreach is structural");
  const auto filtered_catch = llmcc::StructuralEvents(
      "class Q { void M() { try {} catch (Exception e) when (P(e)) {} } }",
      llmcc::Language::kCSharp);
  const auto unfiltered_catch = llmcc::StructuralEvents(
      "class Q { void M() { try {} catch (Exception e) {} } }",
      llmcc::Language::kCSharp);
  llmcc::test::ExpectEq(filtered_catch.size(), unfiltered_catch.size() + 1,
                        "C# catch filter is structural");
  const auto guarded_csharp_switch = llmcc::StructuralEvents(
      "class Q { void M(object v) { switch (v) { case int n when n > 0: "
      "break; } } }",
      llmcc::Language::kCSharp);
  const auto unguarded_csharp_switch = llmcc::StructuralEvents(
      "class Q { void M(object v) { switch (v) { case int n: break; } } }",
      llmcc::Language::kCSharp);
  llmcc::test::ExpectEq(guarded_csharp_switch.size(),
                        unguarded_csharp_switch.size() + 1,
                        "C# switch guard is structural");
  const auto csharp_conditional = llmcc::StructuralEvents(
      "class Q { int M(bool flag) { return flag ? 1 : 0; } }",
      llmcc::Language::kCSharp);
  const auto csharp_unconditional = llmcc::StructuralEvents(
      "class Q { int M(bool flag) { return 1; } }", llmcc::Language::kCSharp);
  llmcc::test::ExpectEq(csharp_conditional.size(),
                        csharp_unconditional.size() + 1,
                        "C# conditional expression is structural");
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
  cpp_names.reserve(extracted_cpp.size());
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
  rust_names.reserve(extracted_rust.size());
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

  const std::string python_docstrings =
      "\"\"\"module doc\ncontinued\"\"\"\n"
      "assigned = \"keep assigned\"\n"
      "\"keep expression\"\n"
      "class C:\n"
      "    r\"\"\"class doc\"\"\"\n"
      "    async def method(self):\n"
      "        u\"method doc\"\n"
      "        value = b\"keep bytes\"\n"
      "        return f\"keep {value}\"\n";
  const llmcc::PreparedSource prepared =
      llmcc::PrepareSource(python_docstrings, llmcc::Language::kPython);
  llmcc::test::Expect(
      prepared.cleaned.find("module doc") == std::string::npos &&
          prepared.cleaned.find("class doc") == std::string::npos &&
          prepared.cleaned.find("method doc") == std::string::npos,
      "actual Python docstrings are removed at every scope");
  for (std::string_view preserved :
       {"keep assigned", "keep expression", "keep bytes", "keep {value}"}) {
    llmcc::test::Expect(prepared.cleaned.find(preserved) != std::string::npos,
                        "non-docstring Python strings are preserved");
  }
  llmcc::test::ExpectEq(std::ranges::count(prepared.cleaned, '\n'),
                        std::ranges::count(python_docstrings, '\n'),
                        "docstring removal preserves every newline");
  const std::size_t assigned = prepared.cleaned.find("assigned");
  llmcc::test::ExpectEq(prepared.original_offsets.at(assigned),
                        python_docstrings.find("assigned"),
                        "prepared source maps retained bytes to originals");
  llmcc::test::ExpectEq(prepared.functions.size(), std::size_t{1},
                        "async function survives docstring removal");
  llmcc::test::ExpectEq(
      prepared.original_offsets.at(prepared.functions.front().start_byte),
      python_docstrings.find("async def"),
      "function span is remapped without reparsing an empty suite");

  const auto only_docstring = llmcc::PrepareSource(
      "\"\"\"documentation only\"\"\"\n", llmcc::Language::kPython);
  llmcc::test::Expect(only_docstring.meaningful_ranges.empty(),
                      "docstring-only module has no semantic content");
  const auto parenthesized_docstrings = llmcc::PrepareSource(
      "(\"module \"  # explanation\n"
      " \"documentation\")\n"
      "def documented():\n"
      "    (\"function documentation\"  # explanation\n"
      "    )\n"
      "    return 1\n",
      llmcc::Language::kPython);
  llmcc::test::Expect(parenthesized_docstrings.cleaned.find("documentation") ==
                              std::string::npos &&
                          parenthesized_docstrings.cleaned.find("return 1") !=
                              std::string::npos,
                      "parenthesized Python docstrings are removed");
  const auto prefixed_expressions = llmcc::PrepareSource(
      "b\"bytes\"\nf\"value {1}\"\n", llmcc::Language::kPython);
  llmcc::test::Expect(
      prefixed_expressions.cleaned.find("bytes") != std::string::npos &&
          prefixed_expressions.cleaned.find("value") != std::string::npos,
      "bytes and f-string expressions are not docstrings");

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
      {"source.c", llmcc::Language::kC},
      {"source.h", llmcc::Language::kC},
      {"kernel.cu", llmcc::Language::kCpp},
      {"kernel.cuh", llmcc::Language::kCpp},
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
  llmcc::test::Expect(
      llmcc::LanguageForExtension(".cu") == llmcc::Language::kCpp &&
          llmcc::LanguageForExtension(".cuh") == llmcc::Language::kCpp,
      "CUDA sources and headers use the C++ grammar");
  const auto cuda = llmcc::PrepareSource(
      "__global__ void tiny(int *out) { *out = threadIdx.x; }\n"
      "void launch(int *out) { tiny<<<1, 1>>>(out); }\n",
      llmcc::Language::kCpp);
  llmcc::test::Expect(
      cuda.functions.size() == 2 && cuda.functions[0].name == "tiny" &&
          cuda.functions[1].name == "launch",
      "CUDA kernel and launch syntax use C++ function extraction");
  llmcc::test::Expect(!llmcc::LanguageForExtension(".jsx").has_value() &&
                          !llmcc::LanguageForExtension(".ts").has_value() &&
                          !llmcc::LanguageForExtension(".tsx").has_value(),
                      "unsupported extensions are omitted");
  llmcc::test::Expect(
      llmcc::InferLanguage("Main.JAVA") == llmcc::Language::kJava &&
          llmcc::InferLanguage("src/Foo.PY") == llmcc::Language::kPython &&
          llmcc::InferLanguage("win\\X.H") == llmcc::Language::kC,
      "extensions are inferred case-insensitively");
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
