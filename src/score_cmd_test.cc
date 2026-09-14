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

  const auto exact = llmcc::PlanScoreWindows(4, 4);
  llmcc::test::ExpectEq(exact.window_count, std::size_t{1},
                        "an input fitting context uses one window");
  const llmcc::ScoreWindow exact_window = llmcc::ScoreWindowAt(exact, 0);
  llmcc::test::ExpectEq(exact_window.token_begin, std::size_t{0},
                        "exact window starts at the first token");
  llmcc::test::ExpectEq(exact_window.token_end, std::size_t{4},
                        "exact window contains all tokens");
  llmcc::test::ExpectEq(exact_window.scored_target_begin, std::size_t{1},
                        "exact window scores after its first token");

  const auto even = llmcc::PlanScoreWindows(10, 4);
  llmcc::test::ExpectEq(even.window_count, std::size_t{4},
                        "even context uses a half-context stride");
  llmcc::test::ExpectEq(llmcc::ScoreWindowAt(even, 1).token_begin,
                        std::size_t{2},
                        "second even window retains half the context");
  llmcc::test::ExpectEq(llmcc::ScoreWindowAt(even, 1).scored_target_begin,
                        std::size_t{4}, "second window emits only new targets");
  llmcc::test::ExpectEq(
      llmcc::ScoreWindowAt(even, even.window_count - 1).token_end,
      std::size_t{10}, "even final window reaches the input end");

  const auto odd = llmcc::PlanScoreWindows(7, 3);
  llmcc::test::ExpectEq(odd.window_count, std::size_t{5},
                        "odd context rounds half-stride down");
  llmcc::test::ExpectEq(llmcc::ScoreWindowAt(odd, 1).token_begin,
                        std::size_t{1},
                        "odd windows make progress by one token");
  llmcc::test::ExpectEq(
      llmcc::ScoreWindowAt(odd, odd.window_count - 1).token_end, std::size_t{7},
      "odd final partial window reaches the input end");
  std::size_t next_target = 1;
  for (std::size_t index = 0; index < odd.window_count; ++index) {
    const llmcc::ScoreWindow window = llmcc::ScoreWindowAt(odd, index);
    llmcc::test::Expect(window.token_end - window.token_begin <= 3,
                        "each planned window is context bounded");
    llmcc::test::Expect(window.token_begin < window.scored_target_begin,
                        "each emitted target has a preceding source token");
    llmcc::test::ExpectEq(window.scored_target_begin, next_target,
                          "planned target ranges are globally contiguous");
    next_target = window.token_end;
  }
  llmcc::test::ExpectEq(next_target, std::size_t{7},
                        "planned targets cover the complete input");

  const auto partial = llmcc::PlanScoreWindows(8, 5);
  const llmcc::ScoreWindow final_partial =
      llmcc::ScoreWindowAt(partial, partial.window_count - 1);
  llmcc::test::ExpectEq(final_partial.token_begin, std::size_t{4},
                        "final partial window keeps the fixed stride");
  llmcc::test::ExpectEq(final_partial.token_end - final_partial.token_begin,
                        std::size_t{4},
                        "final partial window is shorter than context");

  const auto tiny = llmcc::PlanScoreWindows(2, 2);
  llmcc::test::ExpectEq(tiny.window_count, std::size_t{1},
                        "two tokens fit the smallest usable context");

  llmcc::test::ExpectEq(llmcc::PlanScoreWindows(0, 1).window_count,
                        std::size_t{0},
                        "empty input needs no inference context");
  llmcc::test::ExpectEq(llmcc::PlanScoreWindows(1, 1).window_count,
                        std::size_t{0},
                        "a single token needs no inference context");
  rejected = false;
  try {
    static_cast<void>(llmcc::PlanScoreWindows(2, 1));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  llmcc::test::Expect(rejected,
                      "context one rejects input requiring inference");

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
