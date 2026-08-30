#include "src/scoring.h"

#include <cassert>
#include <cmath>
#include <string>
#include <vector>

int main() {
  const std::vector<float> logits = {1.0F, 2.0F, 3.0F};
  const rethink::TokenScore score = rethink::ScoreToken(logits, 2);
  assert(std::abs(score.probability - 0.6652409558) < 1e-9);
  assert(std::abs(std::exp(score.log_probability) - score.probability) < 1e-12);

  assert(rethink::BytesToHex(std::string("A\0", 2)) == "4100");
  assert(rethink::JsonEscapeBytes("a\n\"b") == "a\\n\\\"b");
  assert(rethink::JsonEscapeBytes("\xf0\x9f\x98\x80") == "\xf0\x9f\x98\x80");
  assert(rethink::JsonEscapeBytes("\x80") == "\\ufffd");
  return 0;
}
