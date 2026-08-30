#include "src/scoring.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
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

bool Contains(const std::string& value, std::string_view needle) {
  return value.find(needle) != std::string::npos;
}

double EntropyOrNan(const rethink::TokenScore& score) {
  return score.entropy.value_or(std::numeric_limits<double>::quiet_NaN());
}

}  // namespace

int main() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024;
  constexpr std::uint64_t kGiB = 1024ULL * kMiB;

  Expect(rethink::CheckMemory(kGiB, 2 * kGiB, std::nullopt, 0, false).ok,
         "model fits in host RAM");
  const rethink::MemoryCheckResult host_too_small =
      rethink::CheckMemory(kGiB, 1500 * kMiB, std::nullopt, 0, false);
  Expect(!host_too_small.ok &&
             Contains(host_too_small.error, "host required") &&
             Contains(host_too_small.error, "available"),
         "model does not fit in host RAM");
  Expect(rethink::CheckMemory(kGiB, 512 * kMiB, 1200 * kMiB, 1, false).ok,
         "model fits in GPU VRAM");
  const rethink::MemoryCheckResult gpu_too_small =
      rethink::CheckMemory(kGiB, kGiB, kGiB, -1, false);
  Expect(!gpu_too_small.ok && Contains(gpu_too_small.error, "GPU required") &&
             Contains(gpu_too_small.error, "host required"),
         "model does not fit in GPU VRAM");
  const rethink::MemoryCheckResult no_gpu =
      rethink::CheckMemory(kGiB, 2 * kGiB, std::nullopt, 1, false);
  Expect(!no_gpu.ok && Contains(no_gpu.error, "no GPU backend"),
         "GPU requested without GPU backend");
  Expect(rethink::CheckMemory(std::numeric_limits<std::uint64_t>::max(), 0,
                              std::nullopt, -1, true)
             .ok,
         "memory-check override bypasses failures");

  const std::vector<float> logits = {1.0F, 2.0F, 3.0F};
  const rethink::TokenScore score = rethink::ScoreToken(logits, 2, true);
  Expect(std::abs(score.probability - 0.6652409558) < 1e-9,
         "softmax probability");
  Expect(std::abs(std::exp(score.log_probability) - score.probability) < 1e-12,
         "log probability");
  Expect(score.entropy.has_value(), "entropy requested");
  Expect(std::abs(EntropyOrNan(score) - 0.8323955818) < 1e-9,
         "full-vocabulary entropy");

  const std::vector<float> uniform_logits = {7.0F, 7.0F, 7.0F, 7.0F};
  const rethink::TokenScore uniform =
      rethink::ScoreToken(uniform_logits, 0, true);
  Expect(std::abs(EntropyOrNan(uniform) - std::log(4.0)) < 1e-12,
         "uniform entropy");
  Expect(!rethink::ScoreToken(logits, 2).entropy.has_value(),
         "entropy not requested");

  const std::vector<float> singleton_logits = {42.0F};
  const rethink::TokenScore singleton =
      rethink::ScoreToken(singleton_logits, 0, true);
  Expect(singleton.probability == 1.0 && singleton.log_probability == 0.0 &&
             EntropyOrNan(singleton) == 0.0,
         "single-token vocabulary");

  const float largest = std::numeric_limits<float>::max();
  const std::vector<float> large_logits = {
      largest, largest, std::numeric_limits<float>::lowest()};
  const rethink::TokenScore large = rethink::ScoreToken(large_logits, 0, true);
  Expect(std::isfinite(large.probability) &&
             std::abs(large.probability - 0.5) < 1e-12 &&
             std::isfinite(EntropyOrNan(large)) &&
             std::abs(EntropyOrNan(large) - std::numbers::ln2) < 1e-12,
         "large logits are overflow-safe");

  const std::vector<float> masked_logits = {
      -std::numeric_limits<float>::infinity(), 0.0F};
  const rethink::TokenScore masked =
      rethink::ScoreToken(masked_logits, 1, true);
  Expect(masked.probability == 1.0 && EntropyOrNan(masked) == 0.0,
         "negative-infinity logit masking");

  const std::vector<float> uniform_after_masking = {
      -std::numeric_limits<float>::infinity(), 9.0F, 9.0F, 9.0F,
      -std::numeric_limits<float>::infinity()};
  const rethink::TokenScore masked_uniform =
      rethink::ScoreToken(uniform_after_masking, 2, true);
  Expect(std::abs(masked_uniform.probability - (1.0 / 3.0)) < 1e-12 &&
             std::abs(EntropyOrNan(masked_uniform) - std::log(3.0)) < 1e-12,
         "uniform entropy after masking");

  Expect(rethink::BytesToHex(std::string("A\0", 2)) == "4100", "hex encoding");
  Expect(rethink::JsonEscapeBytes("a\n\"b") == "a\\n\\\"b", "JSON escaping");
  Expect(rethink::JsonEscapeBytes("\xf0\x9f\x98\x80") == "\xf0\x9f\x98\x80",
         "valid UTF-8 preservation");
  Expect(rethink::JsonEscapeBytes("\x80") == "\\ufffd",
         "invalid UTF-8 replacement");
  return 0;
}
