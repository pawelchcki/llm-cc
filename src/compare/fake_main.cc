// A test build of `llm-cc compare` that links no inference backend: it scores
// with the deterministic scorer, so tests run whole comparisons without
// models or GPUs.
#include <iostream>
#include <string>
#include <string_view>

#include "src/compare/cli.h"
#include "src/compare/fake_scorer.h"

int main(int argc, char** argv) {
  if (argc < 2 || std::string_view(argv[1]) != "compare") {
    std::cerr << "error: this test executable only runs `compare`\n";
    return 2;
  }
  try {
    return llmcc::compare::RunCompareCommand(
        argc - 1, argv + 1,
        {.inference_abi = std::string(llmcc::compare::kFakeInferenceAbi),
         .open_session = llmcc::compare::DeterministicScorerFactory()});
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }
}
