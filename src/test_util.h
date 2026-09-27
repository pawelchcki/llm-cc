#ifndef LLM_CC_TEST_UTIL_H_
#define LLM_CC_TEST_UTIL_H_

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace llmcc::test {

inline void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

template <typename Left, typename Right>
void ExpectEq(const Left& left, const Right& right, std::string_view message) {
  if (!(left == right)) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

// Without a sandbox (always on Windows), TEST_TMPDIR lies inside Bazel's
// execroot, which links the workspace's .git and .llm-cc. Stop Git's
// repository discovery just above TEST_TMPDIR so a fixture there is outside
// any repository. Child processes inherit the setting.
inline void StopGitDiscoveryAboveTestTmpdir() {
  const char* temporary = std::getenv("TEST_TMPDIR");
  Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const std::string ceiling =
      std::filesystem::path(temporary).parent_path().string();
#ifdef _WIN32
  Expect(_putenv_s("GIT_CEILING_DIRECTORIES", ceiling.c_str()) == 0,
         "GIT_CEILING_DIRECTORIES is set");
#else
  Expect(setenv("GIT_CEILING_DIRECTORIES", ceiling.c_str(), 1) == 0,
         "GIT_CEILING_DIRECTORIES is set");
#endif
}

}  // namespace llmcc::test

#endif  // LLM_CC_TEST_UTIL_H_
