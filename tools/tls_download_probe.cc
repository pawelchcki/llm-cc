#include <exception>
#include <iostream>

#include "src/download.h"

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  try {
    llmcc::DownloadFile(
        argv[1], argv[2],
        {.show_progress = false, .record_in_model_manifest = false});
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
