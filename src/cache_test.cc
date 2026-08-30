#include "src/cache.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "src/test_util.h"

int main() {
  namespace fs = std::filesystem;
  llmcc::test::ExpectEq(
      llmcc::CacheDirFrom(fs::path("/override"), fs::path("/xdg"),
                          fs::path("/home/user")),
      fs::path("/override"), "cache override precedence");
  llmcc::test::ExpectEq(llmcc::CacheDirFrom(std::nullopt, fs::path("/xdg"),
                                            fs::path("/home/user")),
                        fs::path("/xdg/llm-cc/models"), "XDG cache path");
  llmcc::test::ExpectEq(
      llmcc::CacheDirFrom(std::nullopt, std::nullopt, fs::path("/home/user")),
      fs::path("/home/user/.cache/llm-cc/models"), "home cache path");
  llmcc::test::ExpectEq(llmcc::FormatTimestamp(0),
                        std::string("1970-01-01 00:00:00 UTC"),
                        "epoch formatting");

  const char* temporary = std::getenv("TEST_TMPDIR");
  llmcc::test::Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const fs::path cache = fs::path(temporary) / "cache";
  fs::create_directories(cache);
  std::ofstream(cache / "alpha.gguf") << "model";
  std::ofstream(cache / "ignored.txt") << "text";
  std::ostringstream models;
  llmcc::ListModels(cache, models);
  llmcc::test::Expect(models.str().find("alpha.gguf") != std::string::npos,
                      "GGUF listed");
  llmcc::test::Expect(models.str().find("ignored.txt") == std::string::npos,
                      "non-GGUF omitted");

  std::ofstream(llmcc::PartialPath(cache / "alpha.gguf")) << "partial";
  llmcc::RemoveModel(cache, "alpha.gguf");
  llmcc::test::Expect(!fs::exists(cache / "alpha.gguf"), "model removed");
  llmcc::test::Expect(!fs::exists(cache / "alpha.gguf.partial"),
                      "partial removed");

  bool downloaded = false;
  const auto resolved = llmcc::ResolveModel(std::nullopt, std::nullopt, false,
                                            fs::path(temporary), cache,
                                            [&](const fs::path& target) {
                                              downloaded = true;
                                              std::ofstream(target) << "model";
                                            });
  llmcc::test::Expect(downloaded, "missing default downloaded");
  llmcc::test::ExpectEq(resolved.value_or(fs::path{}),
                        cache / llmcc::kDefaultModelFile,
                        "cached default resolved");
  return 0;
}
