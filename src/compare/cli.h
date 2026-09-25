#ifndef LLM_CC_COMPARE_CLI_H_
#define LLM_CC_COMPARE_CLI_H_

#include <string>

#include "src/scorer_session.h"

namespace llmcc::compare {

// What `compare` needs from the executable it runs in: the inference ABI it
// reports and how it opens a scorer. The release binary passes llama.cpp's;
// llm-cc-compare-fake passes the deterministic scorer.
struct CompareRuntime {
  std::string inference_abi;
  ScorerSessionFactory open_session;
};

// `llm-cc compare ...`; `argv[0]` is "compare".
int RunCompareCommand(int argc, char** argv, const CompareRuntime& runtime);

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_CLI_H_
