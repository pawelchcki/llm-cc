// Child process for subprocess_test. Modes:
//   args ARG...  write each argument followed by a NUL byte
//   cat          copy stdin to stdout and as many bytes to stderr
//   pwd          print the working directory
//   exit N       exit with status N without reading stdin
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

std::string Utf8(const std::filesystem::path& path) {
  const std::u8string value = path.u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

int Run(int argc, char** argv) {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
#endif
  const std::string_view mode = argc > 1 ? argv[1] : "";
  if (mode == "args") {
    for (int index = 2; index < argc; ++index) {
      std::cout << argv[index] << '\0';
    }
    return 0;
  }
  if (mode == "cat") {
    std::string buffer(64 * 1024, '\0');
    std::size_t count = 0;
    while ((count = std::fread(buffer.data(), 1, buffer.size(), stdin)) > 0) {
      std::fwrite(buffer.data(), 1, count, stdout);
      std::fwrite(std::string(count, 'e').data(), 1, count, stderr);
    }
    return 0;
  }
  if (mode == "pwd") {
    std::cout << Utf8(std::filesystem::current_path());
    return 0;
  }
  if (mode == "exit" && argc > 2) {
    return std::atoi(argv[2]);
  }
  return 64;
}

}  // namespace

#if defined(_WIN32)
#include <vector>
int wmain(int argc, wchar_t** wide_argv) {
  std::vector<std::string> encoded;
  for (int index = 0; index < argc; ++index) {
    encoded.push_back(Utf8(std::filesystem::path(wide_argv[index])));
  }
  std::vector<char*> argv;
  for (std::string& argument : encoded) {
    argv.push_back(argument.data());
  }
  return Run(argc, argv.data());
}
#else
int main(int argc, char** argv) { return Run(argc, argv); }
#endif
