#ifndef LLM_CC_RULES_CMD_H_
#define LLM_CC_RULES_CMD_H_

namespace llmcc {

// `llm-cc rules show|check|explain`: inspect selection, language, and
// category decisions without a model. `argv[0]` is "rules".
int RunRulesCommand(int argc, char** argv);

}  // namespace llmcc

#endif  // LLM_CC_RULES_CMD_H_
