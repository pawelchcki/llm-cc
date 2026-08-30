#ifndef LLM_CC_TEST_UTIL_H_
#define LLM_CC_TEST_UTIL_H_

#include <cstdlib>
#include <iostream>
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

}  // namespace llmcc::test

#endif  // LLM_CC_TEST_UTIL_H_
