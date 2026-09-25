#ifndef LLM_CC_COMPARE_CLI_H_
#define LLM_CC_COMPARE_CLI_H_

namespace llmcc::compare {

// `llm-cc compare ...`; `argv[0]` is "compare".
int RunCompareCommand(int argc, char** argv);

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_CLI_H_
