#ifndef LLM_CC_SCORE_CMD_H_
#define LLM_CC_SCORE_CMD_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace llmcc {

inline constexpr std::uint32_t kDefaultContextSize = 128U * 1024U;

struct InferenceOptions {
  std::int32_t gpu_layers = 0;
  std::uint32_t context_size = kDefaultContextSize;
};

std::string ScoreEntropyJsonl(const std::filesystem::path& model,
                              std::string_view input,
                              const InferenceOptions& options = {});
int RunScoreCommand(int argc, char** argv);

}  // namespace llmcc

#endif  // LLM_CC_SCORE_CMD_H_
