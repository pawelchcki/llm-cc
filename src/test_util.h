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

// Sets an environment variable that child processes inherit.
inline void SetEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
#endif
}

// Without a sandbox (always on Windows), TEST_TMPDIR lies inside Bazel's
// execroot, which links the workspace's .git and .llm-cc. Stop Git's
// repository discovery just above `directory` so a fixture there is outside
// any repository.
inline void StopGitDiscoveryAbove(const std::filesystem::path& directory) {
  SetEnvironment("GIT_CEILING_DIRECTORIES", directory.parent_path().string());
}

}  // namespace llmcc::test

#endif  // LLM_CC_TEST_UTIL_H_
