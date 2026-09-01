#ifndef LLM_CC_SCORE_CMD_H_
#define LLM_CC_SCORE_CMD_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "src/backend.h"

namespace llmcc {

inline constexpr std::uint32_t kDefaultContextSize = 128U * 1024U;

struct InferenceOptions {
  std::uint32_t context_size = kDefaultContextSize;
  std::int32_t gpu_layers = 0;
  BackendKind backend = BackendKind::kAuto;
};

class EntropyScorer {
 public:
  EntropyScorer(const std::filesystem::path& model,
                const InferenceOptions& options = {});
  ~EntropyScorer();
  EntropyScorer(const EntropyScorer&) = delete;
  EntropyScorer& operator=(const EntropyScorer&) = delete;
  EntropyScorer(EntropyScorer&&) noexcept;
  EntropyScorer& operator=(EntropyScorer&&) noexcept;

  std::string Score(std::string_view input);

 private:
  class Impl;
  std::unique_ptr<Impl> implementation_;
};

std::string_view InferenceAbi();
std::string_view CompiledBackend();

std::string ScoreEntropyJsonl(const std::filesystem::path& model,
                              std::string_view input,
                              const InferenceOptions& options = {});
int RunScoreCommand(int argc, char** argv);

}  // namespace llmcc

#endif  // LLM_CC_SCORE_CMD_H_
