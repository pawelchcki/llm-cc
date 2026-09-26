// A test build of `llm-cc compare` that links no inference backend. Tests of
// the comparison glue run it where the real executable would be too heavy.
#include <iostream>
#include <string_view>

#include "src/compare/cli.h"

int main(int argc, char** argv) {
  if (argc < 2 || std::string_view(argv[1]) != "compare") {
    std::cerr << "error: this test executable only runs `compare`\n";
    return 2;
  }
  try {
    return llmcc::compare::RunCompareCommand(argc - 1, argv + 1);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }
}
