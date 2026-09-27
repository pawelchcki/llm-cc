#ifndef LLM_CC_COMPARE_TEST_REPOSITORY_H_
#define LLM_CC_COMPARE_TEST_REPOSITORY_H_

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "src/subprocess.h"
#include "src/test_util.h"

namespace llmcc::compare::test {

inline std::string Utf8(const std::filesystem::path& path) {
  const std::u8string value = path.u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

// Runs git in `repository` and returns its trimmed output.
inline std::string Git(const std::filesystem::path& repository,
                       const std::vector<std::string>& arguments,
                       const std::string& input = {}) {
  std::vector<std::string> command = {"git",
                                      "-C",
                                      Utf8(repository),
                                      "-c",
                                      "user.name=Fixture",
                                      "-c",
                                      "user.email=fixture@example.invalid",
                                      "-c",
                                      "commit.gpgsign=false",
                                      "-c",
                                      "core.autocrlf=false",
                                      "-c",
                                      "init.defaultBranch=main"};
  command.insert(command.end(), arguments.begin(), arguments.end());
  ProcessResult result =
      RunProcess(command, input.empty() ? ProcessOptions{}
                                        : ProcessOptions{.stdin_data = input});
  if (result.exit_code != 0) {
    std::string description;
    for (const std::string& argument : arguments) {
      description += argument + " ";
    }
    llmcc::test::Expect(false,
                        "git " + description + "failed: " + result.stderr_data);
  }
  while (!result.stdout_data.empty() && (result.stdout_data.back() == '\n' ||
                                         result.stdout_data.back() == '\r')) {
    result.stdout_data.pop_back();
  }
  return result.stdout_data;
}

inline void Write(const std::filesystem::path& path, std::string_view value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << value;
}

inline std::string Commit(const std::filesystem::path& repository,
                          const std::string& message) {
  Git(repository, {"add", "-A"});
  Git(repository, {"commit", "-q", "--allow-empty", "-m", message});
  return Git(repository, {"rev-parse", "HEAD"});
}

// A symbolic link. Windows fixtures stage it as an index entry, which later
// `add -A` calls would drop, so only POSIX fixtures create the link itself.
inline void AddSymlink(const std::filesystem::path& repository,
                       const std::string& path, const std::string& target) {
#ifdef _WIN32
  const std::string blob =
      Git(repository, {"hash-object", "-w", "--stdin"}, target);
  Git(repository,
      {"update-index", "--add", "--cacheinfo", "120000," + blob + "," + path});
#else
  std::filesystem::create_symlink(target, repository / path);
  Git(repository, {"add", path});
#endif
}

// The fixture the Python comparison tests used: a base commit and a head
// that modifies a file, adds an oversized one and renames into tests/.
struct PipelineRepository {
  std::filesystem::path root;
  std::string base;
  std::string head;
};

inline PipelineRepository MakePipelineRepository(
    const std::filesystem::path& root) {
  std::filesystem::create_directories(root);
  Git(root, {"init", "-q"});
  Write(root / "same.cc", "int x;\n");
  Write(root / "copy.cc", "int x;\n");
  Write(root / "src_test.cc", "int test;\n");
  Write(root / "README.md", "docs\n");
  Write(root / "target.cc", "target.cc");
  Write(root / "api.hpp", "int api();\n");
  Write(root / "move.cc", "int move;\n");
#ifndef _WIN32
  Write(root / "odd\nname.py", "x = 1\n");
#endif
  Git(root, {"add", "-A"});
  AddSymlink(root, "link.cc", "target.cc");
  Git(root, {"commit", "-q", "-m", "base"});
  const std::string base = Git(root, {"rev-parse", "HEAD"});
  Write(root / "same.cc", "int changed;\n");
  Write(root / "large.py", std::string(20, 'x'));
  std::filesystem::create_directories(root / "tests");
  Git(root, {"mv", "move.cc", "tests/move.cc"});
  Git(root, {"add", "same.cc", "large.py"});
  Git(root, {"commit", "-q", "-m", "head"});
  return {.root = root, .base = base, .head = Git(root, {"rev-parse", "HEAD"})};
}

}  // namespace llmcc::compare::test

#endif  // LLM_CC_COMPARE_TEST_REPOSITORY_H_
