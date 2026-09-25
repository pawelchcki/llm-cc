#include "src/subprocess.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

#include <array>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
extern char** environ;  // NOLINT(readability-redundant-declaration)
#endif

namespace llmcc {
namespace {

#if defined(_WIN32)

std::wstring Widen(std::string_view value) {
  if (value.empty()) {
    return {};
  }
  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) {
    throw std::invalid_argument("process argument is not valid UTF-8");
  }
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), size);
  return result;
}

// Quotes one argument for CommandLineToArgvW and the MSVC runtime.
std::wstring WindowsArgument(std::wstring_view value) {
  std::wstring result = L"\"";
  std::size_t backslashes = 0;
  for (wchar_t character : value) {
    if (character == L'\\') {
      ++backslashes;
    } else if (character == L'"') {
      result.append(backslashes * 2 + 1, L'\\');
      result += character;
      backslashes = 0;
    } else {
      result.append(backslashes, L'\\');
      backslashes = 0;
      result += character;
    }
  }
  result.append(backslashes * 2, L'\\');
  return result + L'"';
}

std::wstring FindExecutable(const std::string& name) {
  const std::wstring wide = Widen(name);
  if (name.find_first_of("/\\") != std::string::npos) {
    return wide;
  }
  const DWORD path_size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
  std::vector<wchar_t> search_path(path_size + 1, L'\0');
  if (path_size != 0) {
    GetEnvironmentVariableW(L"PATH", search_path.data(), path_size);
  }
  const DWORD size = SearchPathW(path_size == 0 ? nullptr : search_path.data(),
                                 wide.c_str(), L".exe", 0, nullptr, nullptr);
  if (size == 0) {
    throw ProcessStartError("cannot find executable '" + name + "' on PATH");
  }
  std::vector<wchar_t> executable(size + 1, L'\0');
  if (SearchPathW(path_size == 0 ? nullptr : search_path.data(), wide.c_str(),
                  L".exe", static_cast<DWORD>(executable.size()),
                  executable.data(), nullptr) == 0) {
    throw ProcessStartError("cannot find executable '" + name + "' on PATH");
  }
  return executable.data();
}

class Handle {
 public:
  Handle() = default;
  explicit Handle(HANDLE handle) : handle_(handle) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  Handle& operator=(Handle&& other) noexcept {
    Reset();
    handle_ = std::exchange(other.handle_, {});
    return *this;
  }
  ~Handle() { Reset(); }
  [[nodiscard]] HANDLE get() const { return handle_; }
  HANDLE* out() { return &handle_; }
  void Reset() {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
    handle_ = nullptr;
  }

 private:
  HANDLE handle_ = nullptr;
};

std::string ReadAll(HANDLE handle) {
  std::string output;
  std::array<char, std::size_t{64} * 1024> buffer{};
  DWORD count = 0;
  while (ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                  &count, nullptr) &&
         count != 0) {
    output.append(buffer.data(), count);
  }
  return output;
}

// Handles marked inheritable leak into every child created meanwhile, so
// creation is serialized.
std::mutex& CreationMutex() {
  static std::mutex mutex;
  return mutex;
}

#else

std::string FindExecutable(const std::string& name) {
  if (name.find('/') != std::string::npos) {
    return name;
  }
  const char* environment = std::getenv("PATH");
  const std::string_view search =
      environment != nullptr ? environment : "/usr/bin:/bin";
  std::size_t start = 0;
  while (start <= search.size()) {
    const std::size_t end = std::min(search.find(':', start), search.size());
    const std::string_view directory = search.substr(start, end - start);
    const std::string candidate =
        (directory.empty() ? std::string(".") : std::string(directory)) + "/" +
        name;
    struct stat status{};
    if (stat(candidate.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
        access(candidate.c_str(), X_OK) == 0) {
      return candidate;
    }
    start = end + 1;
  }
  throw ProcessStartError("cannot find executable '" + name + "' on PATH");
}

class Descriptor {
 public:
  Descriptor() = default;
  explicit Descriptor(int descriptor) : descriptor_(descriptor) {}
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  Descriptor(Descriptor&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}
  Descriptor& operator=(Descriptor&& other) noexcept {
    Reset();
    descriptor_ = std::exchange(other.descriptor_, -1);
    return *this;
  }
  ~Descriptor() { Reset(); }
  [[nodiscard]] int get() const { return descriptor_; }
  void Reset() {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
    descriptor_ = -1;
  }

 private:
  int descriptor_ = -1;
};

[[noreturn]] void ThrowErrno(const std::string& message) {
  throw std::runtime_error(message + ": " + std::strerror(errno));
}

// Moves a descriptor above the standard streams and marks it close-on-exec,
// so the child can dup2 every stream without clobbering another.
Descriptor Prepare(int descriptor) {
  Descriptor original(descriptor);
  const int moved = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
  if (moved < 0) {
    ThrowErrno("cannot prepare a process pipe");
  }
  return Descriptor(moved);
}

std::pair<Descriptor, Descriptor> Pipe() {
  std::array<int, 2> descriptors{};
  if (pipe(descriptors.data()) != 0) {
    ThrowErrno("cannot create a process pipe");
  }
  return {Prepare(descriptors[0]), Prepare(descriptors[1])};
}

// Standard input is a socket so a child that exits early turns our writes
// into EPIPE errors instead of a process-wide SIGPIPE.
std::pair<Descriptor, Descriptor> InputChannel() {
  std::array<int, 2> descriptors{};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) != 0) {
    ThrowErrno("cannot create a process input channel");
  }
  Descriptor parent = Prepare(descriptors[0]);
  Descriptor child = Prepare(descriptors[1]);
#if defined(SO_NOSIGPIPE)
  const int enabled = 1;
  setsockopt(parent.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
  shutdown(child.get(), SHUT_WR);
  return {std::move(parent), std::move(child)};
}

std::string ReadAll(int descriptor) {
  std::string output;
  std::array<char, std::size_t{64} * 1024> buffer{};
  while (true) {
    const ssize_t count = read(descriptor, buffer.data(), buffer.size());
    if (count > 0) {
      output.append(buffer.data(), static_cast<std::size_t>(count));
    } else if (count == 0 || errno != EINTR) {
      break;
    }
  }
  return output;
}

void WriteAll(int descriptor, std::string_view data) {
#if defined(MSG_NOSIGNAL)
  constexpr int kFlags = MSG_NOSIGNAL;
#else
  constexpr int kFlags = 0;
#endif
  while (!data.empty()) {
    const ssize_t count = send(descriptor, data.data(), data.size(), kFlags);
    if (count > 0) {
      data.remove_prefix(static_cast<std::size_t>(count));
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      // The child stopped reading; its exit status reports why.
      return;
    }
  }
}

// Only async-signal-safe calls between fork and exec.
[[noreturn]] void RunChild(int input, int output, int error, int status,
                           const char* directory, const char* executable,
                           char* const* arguments) {
  if (dup2(input, STDIN_FILENO) < 0 || dup2(output, STDOUT_FILENO) < 0 ||
      dup2(error, STDERR_FILENO) < 0 ||
      (directory != nullptr && chdir(directory) != 0)) {
    const int failure = errno;
    static_cast<void>(write(status, &failure, sizeof(failure)));
    _exit(127);
  }
  execve(executable, arguments, environ);
  const int failure = errno;
  static_cast<void>(write(status, &failure, sizeof(failure)));
  _exit(127);
}

#endif

std::string Describe(const std::vector<std::string>& argv) {
  return "'" + argv.front() + "'";
}

}  // namespace

#if defined(_WIN32)

ProcessResult RunProcess(const std::vector<std::string>& argv,
                         const ProcessOptions& options) {
  if (argv.empty()) {
    throw std::invalid_argument("a process needs at least an executable");
  }
  const std::wstring executable = FindExecutable(argv.front());
  std::wstring command_line = WindowsArgument(executable);
  for (std::size_t index = 1; index < argv.size(); ++index) {
    command_line += L" " + WindowsArgument(Widen(argv[index]));
  }
  const std::wstring directory =
      options.cwd.has_value() ? options.cwd->wstring() : std::wstring();
  SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
  Handle input_read;
  Handle input_write;
  Handle output_read;
  Handle output_write;
  Handle error_read;
  Handle error_write;
  PROCESS_INFORMATION process{};
  {
    const std::scoped_lock lock(CreationMutex());
    if (!CreatePipe(output_read.out(), output_write.out(), &inheritable, 0) ||
        !SetHandleInformation(output_read.get(), HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(error_read.out(), error_write.out(), &inheritable, 0) ||
        !SetHandleInformation(error_read.get(), HANDLE_FLAG_INHERIT, 0)) {
      throw std::runtime_error("cannot create process pipes");
    }
    if (options.stdin_data.has_value()) {
      if (!CreatePipe(input_read.out(), input_write.out(), &inheritable, 0) ||
          !SetHandleInformation(input_write.get(), HANDLE_FLAG_INHERIT, 0)) {
        throw std::runtime_error("cannot create process input pipe");
      }
    } else {
      input_read = Handle(CreateFileW(
          L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
          &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input_read.get();
    startup.hStdOutput = output_write.get();
    startup.hStdError = error_write.get();
    if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr,
                        nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        directory.empty() ? nullptr : directory.c_str(),
                        &startup, &process)) {
      throw ProcessStartError("cannot start " + Describe(argv) +
                              ": Windows error " +
                              std::to_string(GetLastError()));
    }
    input_read.Reset();
    output_write.Reset();
    error_write.Reset();
  }
  Handle process_handle(process.hProcess);
  Handle thread_handle(process.hThread);
  std::thread writer;
  if (options.stdin_data.has_value()) {
    writer = std::thread([&] {
      std::string_view data = *options.stdin_data;
      while (!data.empty()) {
        const DWORD chunk =
            static_cast<DWORD>(std::min<std::size_t>(data.size(), 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(input_write.get(), data.data(), chunk, &written,
                       nullptr) ||
            written == 0) {
          break;
        }
        data.remove_prefix(written);
      }
      input_write.Reset();
    });
  }
  ProcessResult result;
  std::thread error_reader(
      [&] { result.stderr_data = ReadAll(error_read.get()); });
  result.stdout_data = ReadAll(output_read.get());
  if (writer.joinable()) {
    writer.join();
  }
  error_reader.join();
  WaitForSingleObject(process_handle.get(), INFINITE);
  DWORD status = 1;
  GetExitCodeProcess(process_handle.get(), &status);
  result.exit_code = static_cast<int>(status);
  return result;
}

#else

ProcessResult RunProcess(const std::vector<std::string>& argv,
                         const ProcessOptions& options) {
  if (argv.empty()) {
    throw std::invalid_argument("a process needs at least an executable");
  }
  const std::string executable = FindExecutable(argv.front());
  std::vector<std::string> arguments = argv;
  std::vector<char*> pointers;
  pointers.reserve(arguments.size() + 1);
  for (std::string& argument : arguments) {
    pointers.push_back(argument.data());
  }
  pointers.push_back(nullptr);
  const std::string directory =
      options.cwd.has_value() ? options.cwd->string() : std::string();

  auto [input_parent, input_child] =
      options.stdin_data.has_value()
          ? InputChannel()
          : std::pair{Descriptor(), Prepare(open("/dev/null", O_RDONLY))};
  auto [output_parent, output_child] = Pipe();
  auto [error_parent, error_child] = Pipe();
  auto [status_parent, status_child] = Pipe();

  const pid_t pid = fork();
  if (pid < 0) {
    ThrowErrno("cannot start " + Describe(argv));
  }
  if (pid == 0) {
    RunChild(input_child.get(), output_child.get(), error_child.get(),
             status_child.get(),
             directory.empty() ? nullptr : directory.c_str(),
             executable.c_str(), pointers.data());
  }
  input_child.Reset();
  output_child.Reset();
  error_child.Reset();
  status_child.Reset();

  const auto wait = [pid] {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
      if (errno != EINTR) {
        return -1;
      }
    }
    return WIFEXITED(status)     ? WEXITSTATUS(status)
           : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                 : -1;
  };
  int failure = 0;
  const std::string status = ReadAll(status_parent.get());
  if (status.size() == sizeof(failure)) {
    std::memcpy(&failure, status.data(), sizeof(failure));
    wait();
    throw ProcessStartError("cannot start " + Describe(argv) + ": " +
                            std::strerror(failure));
  }

  std::thread writer;
  if (options.stdin_data.has_value()) {
    writer = std::thread([&input = input_parent, &options] {
      WriteAll(input.get(), *options.stdin_data);
      input.Reset();
    });
  }
  ProcessResult result;
  std::thread error_reader([&result, &error = error_parent] {
    result.stderr_data = ReadAll(error.get());
  });
  result.stdout_data = ReadAll(output_parent.get());
  if (writer.joinable()) {
    writer.join();
  }
  error_reader.join();
  result.exit_code = wait();
  return result;
}

#endif

}  // namespace llmcc
