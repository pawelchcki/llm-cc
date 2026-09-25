#include "src/subprocess.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "src/test_util.h"

namespace {

std::vector<std::string> SplitNul(const std::string& text) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\0', start);
    fields.push_back(text.substr(start, end - start));
    start = end == std::string::npos ? text.size() : end + 1;
  }
  return fields;
}

std::string Utf8(const std::filesystem::path& path) {
  const std::u8string value = path.u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape)
  using llmcc::test::Expect;
  using llmcc::test::ExpectEq;
  const char* test_srcdir = std::getenv("TEST_SRCDIR");
  const char* test_tmpdir = std::getenv("TEST_TMPDIR");
  Expect(argc == 2 && test_srcdir != nullptr && test_tmpdir != nullptr,
         "helper and test directories are provided");
  const std::string helper = Utf8(std::filesystem::path(test_srcdir) / argv[1]);

  // Arguments reach the child byte for byte, without a shell.
  const std::vector<std::string> arguments = {
      "plain",
      "with space",
      "",
      "quote\"inside",
      "trailing\\",
      "back\\\\slash \\\" mix",
      "$HOME `id` ; | & *",
      "za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 \xE2\x9C\x93",
  };
  std::vector<std::string> command = {helper, "args"};
  command.insert(command.end(), arguments.begin(), arguments.end());
  const auto echoed = llmcc::RunProcess(command);
  ExpectEq(echoed.exit_code, 0, "argument echo succeeds");
  ExpectEq(SplitNul(echoed.stdout_data), arguments,
           "arguments with spaces, quotes and Unicode survive unchanged");

  // Filling both output pipes while input is still being written would
  // deadlock a sequential implementation.
  std::string large(1024 * 1024 + 17, '\0');
  for (std::size_t index = 0; index < large.size(); ++index) {
    large[index] = static_cast<char>(index * 31 % 251);
  }
  const auto copied = llmcc::RunProcess({helper, "cat"}, {.stdin_data = large});
  ExpectEq(copied.exit_code, 0, "cat succeeds");
  Expect(copied.stdout_data == large, "one mebibyte of input round-trips");
  ExpectEq(copied.stderr_data.size(), large.size(),
           "standard error is drained concurrently");

  // A child that exits without reading its input must not kill the parent.
  const auto ignored = llmcc::RunProcess(
      {helper, "exit", "3"},
      {.stdin_data = std::string(std::size_t{4} * 1024 * 1024, 'x')});
  ExpectEq(ignored.exit_code, 3, "exit status is reported");

  const auto empty = llmcc::RunProcess({helper, "cat"});
  Expect(empty.exit_code == 0 && empty.stdout_data.empty(),
         "standard input defaults to the null device");

  const std::filesystem::path directory =
      std::filesystem::path(test_tmpdir) / "working directory \xE2\x9C\x93";
  std::filesystem::create_directories(directory);
  const auto working = llmcc::RunProcess({helper, "pwd"}, {.cwd = directory});
  ExpectEq(
      std::filesystem::canonical(std::filesystem::u8path(working.stdout_data)),
      std::filesystem::canonical(directory),
      "the child starts in the requested directory");

  try {
    static_cast<void>(
        llmcc::RunProcess({"llm-cc-definitely-missing-executable"}));
    Expect(false, "a missing executable fails to start");
  } catch (const llmcc::ProcessStartError& error) {
    Expect(std::string(error.what())
                   .find("llm-cc-definitely-missing-executable") !=
               std::string::npos,
           "the start error names the executable");
  }
  try {
    static_cast<void>(llmcc::RunProcess({helper, "pwd"},
                                        {.cwd = directory / "does-not-exist"}));
    Expect(false, "a missing working directory fails to start");
  } catch (const llmcc::ProcessStartError&) {  // NOLINT(bugprone-empty-catch)
  } catch (const std::runtime_error&) {        // NOLINT(bugprone-empty-catch)
    // Windows reports the directory through CreateProcess as well.
  }
  return 0;
}
