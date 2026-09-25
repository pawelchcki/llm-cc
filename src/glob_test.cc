#include "src/glob.h"

#include <stdexcept>
#include <string>
#include <string_view>

#include "src/test_util.h"

namespace {

bool Matches(std::string_view pattern, std::string_view path) {
  return llmcc::Glob::Compile(pattern).Matches(path);
}

bool Prunes(std::string_view pattern, std::string_view directory) {
  return llmcc::Glob::Compile(pattern).PrunesDirectory(directory);
}

void ExpectInvalid(std::string_view pattern) {
  try {
    static_cast<void>(llmcc::Glob::Compile(pattern));
  } catch (const std::invalid_argument&) {
    return;
  }
  llmcc::test::Expect(false,
                      "invalid glob is rejected: " + std::string(pattern));
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  using llmcc::test::Expect;

  // Single-segment wildcards are anchored and never cross '/'.
  Expect(Matches("*.cc", "a.cc"), "star matches within a segment");
  Expect(!Matches("*.cc", "src/a.cc"), "star does not cross a separator");
  Expect(!Matches("src", "src/a.cc"), "patterns are anchored at the end");
  Expect(!Matches("a.cc", "src/a.cc"), "patterns are anchored at the root");
  Expect(
      Matches("*_test.*", "a_test.cc") && !Matches("*_test.*", "src/a_test.cc"),
      "a bare file glob matches only at the root");
  Expect(
      Matches("?.c", "a.c") && !Matches("?.c", "ab.c") && !Matches("?", "a/b"),
      "question mark matches one character within a segment");
  Expect(Matches("?.c", "\xC3\xA9.c"),
         "question mark matches one UTF-8 character");
  Expect(Matches("*", ".hidden"), "star matches a leading dot");
  Expect(Matches("a**b", "axyb") && !Matches("a**b", "ax/b"),
         "double star inside a segment is a plain star");

  // Whole-segment globstars.
  Expect(Matches("**/*.cc", "a.cc") && Matches("**/*.cc", "src/a.cc") &&
             Matches("**/*.cc", "src/x/a.cc"),
         "leading globstar matches zero or more directories");
  Expect(Matches("a/**/b", "a/b") && Matches("a/**/b", "a/x/b") &&
             Matches("a/**/b", "a/x/y/b") && !Matches("a/**/b", "a/x/c"),
         "inner globstar matches zero or more directories");
  Expect(Matches("tools/**", "tools/a.py") &&
             Matches("tools/**", "tools/x/y.py") &&
             !Matches("tools/**", "tools") &&
             !Matches("tools/**", "toolsx/a.py"),
         "trailing globstar matches contents, not the directory itself");
  Expect(Matches("**/vendor/**", "vendor/a.go") &&
             Matches("**/vendor/**", "x/vendor/y/a.go") &&
             !Matches("**/vendor/**", "vendor") &&
             !Matches("**/vendor/**", "xvendor/a.go"),
         "surrounding globstars find a directory at any depth");
  Expect(Matches("**", "a") && Matches("**", "a/b/c"),
         "a lone globstar matches every path");
  Expect(Matches("**/bin/**/*.cs", "bin/Hidden.cs") &&
             Matches("**/bin/**/*.cs", "x/bin/y/Hidden.cs") &&
             !Matches("**/bin/**/*.cs", "bin/tool.csx"),
         "globstars compose with a file suffix");

  // Sets and escapes.
  Expect(Matches("[abc].c", "b.c") && !Matches("[abc].c", "d.c"),
         "a set matches its members");
  Expect(Matches("[!abc].c", "d.c") && Matches("[^abc].c", "d.c") &&
             !Matches("[!abc].c", "a.c"),
         "a negated set matches other characters");
  Expect(Matches("[a-c]x", "bx") && !Matches("[a-c]x", "dx"),
         "a range matches inclusively");
  Expect(Matches("[]]", "]") && Matches("[!]]", "a"),
         "a leading bracket is a set member");
  Expect(!Matches("[!a]", "/"), "a negated set never matches a separator");
  Expect(Matches("\\*.c", "*.c") && !Matches("\\*.c", "a.c"),
         "a backslash escapes a wildcard");

  // Git paths keep their case on every host.
  Expect(!Matches("*.CC", "a.cc") && !Matches("tests/**", "Tests/a.py"),
         "matching is case-sensitive");

  ExpectInvalid("");
  ExpectInvalid("/absolute/**");
  ExpectInvalid("directory/");
  ExpectInvalid("a//b");
  ExpectInvalid("[abc");
  ExpectInvalid("trailing\\");
  ExpectInvalid("./a");
  ExpectInvalid("a/../b");

  // Pruning answers for whole directories.
  Expect(Prunes("third_party/**", "third_party") &&
             Prunes("third_party/**", "third_party/x") &&
             !Prunes("third_party/**", "third") &&
             !Prunes("third_party/**", "src/third_party"),
         "a rooted directory glob prunes that directory");
  Expect(Prunes("**/vendor/**", "vendor") &&
             Prunes("**/vendor/**", "a/b/vendor") &&
             !Prunes("**/vendor/**", "a/vendorx"),
         "an unrooted directory glob prunes it at any depth");
  Expect(Prunes("bazel-*/**", "bazel-out") && !Prunes("bazel-*/**", "src"),
         "a wildcard directory glob prunes matching directories");
  Expect(!Prunes("*.cc", "src") && !Prunes("**/*.pb.*", "gen") &&
             !Prunes("**/bin/**/*.cs", "bin"),
         "file globs never prune a directory");
  Expect(Prunes("**", "anything"), "a lone globstar prunes everything");
  return 0;
}
