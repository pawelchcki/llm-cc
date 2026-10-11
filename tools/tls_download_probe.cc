#include <exception>
#include <iostream>
#include <memory>
#include <string_view>

#include "src/download.h"
#include "src/progress.h"

int main(int argc, char** argv) {
  if (argc != 3 && argc != 4) return 2;
  try {
    std::unique_ptr<llmcc::ProgressReporter> progress;
    std::unique_ptr<llmcc::CliSession> session;
    if (argc == 4 && std::string_view(argv[3]) != "standalone") {
      progress = std::make_unique<llmcc::ProgressReporter>(argv[3]);
      session = std::make_unique<llmcc::CliSession>(*progress, true, false);
    }
    llmcc::DownloadFile(argv[1], argv[2],
                        {.noun = "model",
                         .show_progress = argc == 4,
                         .record_in_model_manifest = false});
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
