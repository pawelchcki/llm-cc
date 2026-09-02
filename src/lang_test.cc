#include "src/lang.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "src/test_util.h"

namespace {

std::string Read(std::string_view path) {
  std::ifstream input(std::string(path), std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void CheckComments(std::string_view path, llmcc::Language language) {
  const std::string source = Read(path);
  const auto [stripped, map] = llmcc::StripComments(source, language);
  llmcc::test::Expect(stripped.find("block comment") == std::string::npos,
                      "block comment removed");
  llmcc::test::ExpectEq(map.size(), stripped.size() + 1,
                        "offset map has every boundary");
  llmcc::test::ExpectEq(map.back(), source.size(), "offset map ends at source");
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  CheckComments("testdata/lang/comments.rs", llmcc::Language::kRust);
  CheckComments("testdata/lang/comments.c", llmcc::Language::kC);
  CheckComments("testdata/lang/comments.cc", llmcc::Language::kCpp);

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

  llmcc::test::ExpectEq(llmcc::LineStarts("one\ntwo\n"),
                        std::vector<std::size_t>({0, 4, 8}), "line starts");
  llmcc::test::ExpectEq(llmcc::LineStarts(""), std::vector<std::size_t>({0}),
                        "empty source has first line");

  llmcc::test::ExpectEq(llmcc::InferLanguage("source.rs"),
                        llmcc::Language::kRust, "infer Rust");
  llmcc::test::ExpectEq(llmcc::ParseLanguage("c++"), llmcc::Language::kCpp,
                        "parse C++ alias");
  return 0;
}
