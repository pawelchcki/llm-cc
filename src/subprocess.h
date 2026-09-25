#ifndef LLM_CC_SUBPROCESS_H_
#define LLM_CC_SUBPROCESS_H_

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace llmcc {

struct ProcessOptions {
  std::optional<std::filesystem::path> cwd;
  // Written to the child's standard input, which is otherwise the null
  // device.
  std::optional<std::string> stdin_data;
};

struct ProcessResult {
  // The exit status, or 128 plus the signal number that ended the child.
  int exit_code = 0;
  std::string stdout_data;
  std::string stderr_data;
};

// The child could not be started, for example because the executable is not
// on PATH. A child that starts and fails reports its exit code instead.
class ProcessStartError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Runs `argv` without a shell: argv[0] is looked up on PATH unless it names a
// path, and each argument reaches the child unchanged. Standard input is
// written while both output streams are drained, so no pipe can deadlock.
ProcessResult RunProcess(const std::vector<std::string>& argv,
                         const ProcessOptions& options = {});

}  // namespace llmcc

#endif  // LLM_CC_SUBPROCESS_H_
