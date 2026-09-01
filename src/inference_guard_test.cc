#include "src/inference_guard.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <optional>
#include <string>

#include "src/test_util.h"

int main() {
  llmcc::InferenceGuard cpu_guard("cpu");

  llmcc::test::Expect(
      !llmcc::CodexSandboxGpuWarning(std::nullopt, "seatbelt").has_value(),
      "missing GPU does not imply sandbox interference");
  llmcc::test::Expect(
      !llmcc::CodexSandboxGpuWarning(0, nullptr).has_value(),
      "zero GPU memory outside Codex keeps the ordinary diagnostic");
  const auto warning = llmcc::CodexSandboxGpuWarning(0, "seatbelt");
  llmcc::test::Expect(
      warning.has_value() &&
          warning->find("outside the sandbox") != std::string::npos,
      "Codex sandbox with zero GPU memory gives actionable guidance");

  int start[2];
  int acquired[2];
  llmcc::test::Expect(pipe(start) == 0 && pipe(acquired) == 0,
                      "lock test pipes created");
  const std::string backend = "test-" + std::to_string(getpid());
  const pid_t child = fork();
  llmcc::test::Expect(child >= 0, "lock test child created");
  if (child == 0) {
    close(start[1]);
    close(acquired[0]);
    char signal = 0;
    if (read(start[0], &signal, 1) != 1 || write(acquired[1], "r", 1) != 1) {
      _exit(2);
    }
    llmcc::InferenceGuard child_guard(backend);
    _exit(write(acquired[1], "a", 1) == 1 ? 0 : 3);
  }
  close(start[0]);
  close(acquired[1]);
  {
    llmcc::InferenceGuard parent_guard(backend);
    llmcc::test::Expect(write(start[1], "s", 1) == 1,
                        "child told to acquire held lock");
    char signal = 0;
    llmcc::test::Expect(read(acquired[0], &signal, 1) == 1 && signal == 'r',
                        "child started lock acquisition");
    llmcc::test::Expect(fcntl(acquired[0], F_SETFL, O_NONBLOCK) == 0,
                        "result pipe made nonblocking");
    usleep(100000);
    llmcc::test::Expect(read(acquired[0], &signal, 1) == -1 &&
                            (errno == EAGAIN || errno == EWOULDBLOCK),
                        "second process waits while lock is held");
  }
  llmcc::test::Expect(fcntl(acquired[0], F_SETFL, 0) == 0,
                      "result pipe restored to blocking mode");
  char signal = 0;
  llmcc::test::Expect(read(acquired[0], &signal, 1) == 1 && signal == 'a',
                      "second process acquires released lock");
  int status = 0;
  llmcc::test::Expect(waitpid(child, &status, 0) == child &&
                          WIFEXITED(status) && WEXITSTATUS(status) == 0,
                      "lock test child exits successfully");
  return 0;
}
