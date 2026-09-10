#include "src/score_cmd.h"

#include <filesystem>
#include <stdexcept>

#include "src/test_util.h"

int main() {  // NOLINT(bugprone-exception-escape)
  bool rejected = false;
  try {
    llmcc::EntropyScorer scorer(
        std::filesystem::path("missing.gguf"),
        {.batch_size = 0, .no_download = true, .fetch_backend = false});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  llmcc::test::Expect(rejected,
                      "library API rejects a zero inference batch size");

  llmcc::test::ExpectEq(llmcc::ContextRequestCapacity(4096, 64, 1),
                        std::uint32_t{64}, "small input requests one batch");
  llmcc::test::ExpectEq(llmcc::ContextRequestCapacity(4096, 64, 65),
                        std::uint32_t{65},
                        "context requests do not grow geometrically");
  llmcc::test::ExpectEq(llmcc::ContextRequestCapacity(4096, 64, 4096),
                        std::uint32_t{4096},
                        "context limit boundary is accepted");

  rejected = false;
  try {
    llmcc::EntropyScorer scorer(std::filesystem::path("missing.gguf"),
                                {.flash_attention = llmcc::FlashAttention::kOff,
                                 .kv_cache_type = llmcc::KvCacheType::kQ8_0,
                                 .no_download = true,
                                 .fetch_backend = false});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  llmcc::test::Expect(
      rejected, "quantized K/V with disabled Flash Attention is rejected");
  return 0;
}
