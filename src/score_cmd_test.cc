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
  return 0;
}
