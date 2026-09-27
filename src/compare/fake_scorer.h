#ifndef LLM_CC_COMPARE_FAKE_SCORER_H_
#define LLM_CC_COMPARE_FAKE_SCORER_H_

#include <atomic>
#include <memory>
#include <string>

#include "src/scorer_session.h"

namespace llmcc::compare {

// The inference ABI the deterministic scorer reports.
inline constexpr std::string_view kFakeInferenceAbi =
    "deterministic-test/entropy-v1";

// Sessions whose "model" is any file, hashed like a real one, and whose
// entropies are a pure function of the source bytes. Tests and the
// llm-cc-compare-fake binary run the whole comparison with it and no
// inference runtime. `scored` counts files that reached the scorer.
ScorerSessionFactory DeterministicScorerFactory(
    std::shared_ptr<std::atomic<int>> scored = {});

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_FAKE_SCORER_H_
