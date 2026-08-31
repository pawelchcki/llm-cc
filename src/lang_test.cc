#include "src/lang.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

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

int main() {
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

  llmcc::test::ExpectEq(llmcc::InferLanguage("source.rs"),
                        llmcc::Language::kRust, "infer Rust");
  llmcc::test::ExpectEq(llmcc::ParseLanguage("c++"), llmcc::Language::kCpp,
                        "parse C++ alias");
  return 0;
}
