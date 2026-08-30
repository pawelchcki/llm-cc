#include "src/scoring.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

}  // namespace

int main() {
  const std::vector<float> logits = {1.0F, 2.0F, 3.0F};
  const rethink::TokenScore score = rethink::ScoreToken(logits, 2);
  Expect(std::abs(score.probability - 0.6652409558) < 1e-9,
         "softmax probability");
  Expect(std::abs(std::exp(score.log_probability) - score.probability) < 1e-12,
         "log probability");

  Expect(rethink::BytesToHex(std::string("A\0", 2)) == "4100",
         "hex encoding");
  Expect(rethink::JsonEscapeBytes("a\n\"b") == "a\\n\\\"b",
         "JSON escaping");
  Expect(rethink::JsonEscapeBytes("\xf0\x9f\x98\x80") == "\xf0\x9f\x98\x80",
         "valid UTF-8 preservation");
  Expect(rethink::JsonEscapeBytes("\x80") == "\\ufffd",
         "invalid UTF-8 replacement");
  return 0;
}
