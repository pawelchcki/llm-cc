#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include "src/progress.h"

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::string_view mode(argv[1]);
  try {
    llmcc::ProgressReporter progress("always");
    llmcc::CliSession session(progress, mode == "yes", mode == "offline");
    llmcc::ReportPhase("acquiring model");
    llmcc::ConfirmDownload("https://example.invalid/model", "model.gguf",
                           "model", 4);
    if (mode == "twice") {
      llmcc::ConfirmDownload("https://example.invalid/backend",
                             "backend.bundle", "backend", 4);
    }
    llmcc::ReportPhase("consent accepted");
    std::string source;
    std::getline(std::cin, source);
    std::cout << "accepted stdin=" << source << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
