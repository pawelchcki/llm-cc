#include "src/download.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "src/cache.h"
#include "src/test_util.h"

int main() {
  namespace fs = std::filesystem;
  const fs::path root = std::getenv("TEST_TMPDIR");
  const fs::path target = root / "models/model.gguf";
  std::istringstream first("model data");
  llmcc::StreamDownload(first, target, 0, 10);
  std::ifstream completed(target, std::ios::binary);
  std::string contents((std::istreambuf_iterator<char>(completed)), {});
  llmcc::test::ExpectEq(contents, std::string("model data"),
                        "completed download renamed");

  const fs::path resumed = root / "resumed.gguf";
  std::ofstream(llmcc::PartialPath(resumed), std::ios::binary) << "first ";
  std::istringstream second("second");
  llmcc::StreamDownload(second, resumed, 6, 12);
  std::ifstream resumed_input(resumed, std::ios::binary);
  contents.assign(std::istreambuf_iterator<char>(resumed_input), {});
  llmcc::test::ExpectEq(contents, std::string("first second"),
                        "resume appends");
  return 0;
}
