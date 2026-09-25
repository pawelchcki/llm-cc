#ifndef LLM_CC_SCORE_CMD_H_
#define LLM_CC_SCORE_CMD_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend.h"
#include "src/inference_options.h"
#include "src/jsonl.h"

namespace llmcc {

std::uint32_t ContextRequestCapacity(std::uint32_t context_limit,
                                     std::uint32_t batch_size,
                                     std::uint32_t required_tokens);

struct InferenceOptions {
  std::uint32_t context_size = kDefaultContextSize;
  std::int32_t gpu_layers = 0;
  BackendKind backend = BackendKind::kAuto;
  std::uint32_t batch_size = kDefaultBatchSize;
  EntropyReduction entropy_reduction = EntropyReduction::kAuto;
  FlashAttention flash_attention = FlashAttention::kOn;
  KvCacheType kv_cache_type = KvCacheType::kQ8_0;
  bool kv_offload = true;
  bool backend_diagnostics = false;
  std::function<void(std::size_t, std::size_t)> progress;
  std::optional<std::filesystem::path> backend_directory;
  bool no_download = false;
  bool fetch_backend = true;
};

struct ScoreWindow {
  std::size_t token_begin;
  std::size_t token_end;
  std::size_t scored_target_begin;
};

struct ScoreWindowPlan {
  std::size_t token_count;
  std::uint32_t context_size;
  std::uint32_t stride;
  std::size_t window_count;
};

// Plans independent, overlapping inference windows in constant space.
ScoreWindowPlan PlanScoreWindows(std::size_t token_count,
                                 std::uint32_t context_size);
// token_end is exclusive; scored_target_begin is the first target emitted by
// this window. Throws std::out_of_range for an invalid window index.
ScoreWindow ScoreWindowAt(const ScoreWindowPlan& plan,
                          std::size_t window_index);

struct ScoreMetadata {
  std::size_t source_tokens = 0;
  std::uint32_t context_size = 0;
  std::uint32_t window_stride = 0;
  std::size_t window_count = 0;
};

struct EntropyScoreResult {
  std::vector<EntropyRecord> records;
  ScoreMetadata metadata;
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
  std::vector<EntropyRecord> ScoreRecords(std::string_view input);
  EntropyScoreResult ScoreRecordsWithMetadata(std::string_view input);

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
