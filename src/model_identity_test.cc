#include "src/model_identity.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "src/entropy_cache.h"
#include "src/sha256.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "src/test_util.h"

namespace fs = std::filesystem;

namespace {

void Write(const fs::path& path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << contents;
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  const char* temporary = std::getenv("TEST_TMPDIR");
  llmcc::test::Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const fs::path root = fs::path(temporary) / "identity";
  fs::create_directories(root);
  setenv("LLM_CC_ENTROPY_CACHE_DIR", root.c_str(), 1);

  llmcc::test::ExpectEq(llmcc::Sha256Hex(""),
                        std::string("e3b0c44298fc1c149afbf4c8996fb924"
                                    "27ae41e4649b934ca495991b7852b855"),
                        "SHA-256 empty vector");
  llmcc::test::ExpectEq(llmcc::Sha256Hex("abc"),
                        std::string("ba7816bf8f01cfea414140de5dae2223"
                                    "b00361a396177a9cb410ff61f20015ad"),
                        "SHA-256 abc vector");

  const fs::path model = root / "model.gguf";
  Write(model, "first model bytes");
  const auto first = llmcc::InspectModel(model, "abi", "cpu", 32);
  llmcc::test::Expect(first.content_digest.size() == 64, "model has digest");
  const auto memo =
      root / "model-digests" /
      (llmcc::Sha256Hex(fs::canonical(model).generic_string()) + ".json");
  llmcc::test::Expect(fs::exists(memo), "digest memo written");
#if !defined(_WIN32)
  const auto permissions = fs::status(memo).permissions();
  llmcc::test::Expect(
      (permissions & (fs::perms::others_read | fs::perms::others_write |
                      fs::perms::others_exec)) == fs::perms::none,
      "digest memo is private");
#endif
  const auto memo_time = fs::last_write_time(memo);
  const auto again = llmcc::InspectModel(model, "abi", "cpu", 32);
  llmcc::test::ExpectEq(again.content_digest, first.content_digest,
                        "stable model reuses digest");
  llmcc::test::ExpectEq(fs::last_write_time(memo), memo_time,
                        "memo hit does not rewrite memo");

  const fs::path copy = root / "copy.gguf";
  fs::copy_file(model, copy);
  const auto copied = llmcc::InspectModel(copy, "abi", "cpu", 32);
  llmcc::test::ExpectEq(copied.content_digest, first.content_digest,
                        "identical copy has same digest");
  llmcc::test::ExpectEq(llmcc::EntropyCacheKey("source", copied),
                        llmcc::EntropyCacheKey("source", first),
                        "identical copy has same cache key");

  const auto preserved_mtime = fs::last_write_time(model);
  Write(model, "second modelbytes");  // Same size as the original contents.
  fs::last_write_time(model, preserved_mtime);
  const auto replaced = llmcc::InspectModel(model, "abi", "cpu", 32);
  llmcc::test::Expect(replaced.content_digest != first.content_digest,
                      "same-size preserved-mtime replacement invalidates memo");

  Write(memo, "not json");
  const auto malformed = llmcc::InspectModel(model, "abi", "cpu", 32);
  llmcc::test::ExpectEq(malformed.content_digest, replaced.content_digest,
                        "malformed memo falls back to hash");
  const auto uncached =
      llmcc::InspectModel(model, "abi", "cpu", 32, 64, "auto", "host", false);
  llmcc::test::Expect(uncached.content_digest.empty(),
                      "disabled cache skips model digest");

  const fs::path replacement = root / "replacement.gguf";
  Write(replacement, "third modelbytes!");  // Also 17 bytes.
  fs::last_write_time(replacement, preserved_mtime);
  fs::rename(replacement, model);
  const auto new_inode = llmcc::InspectModel(model, "abi", "cpu", 32);
  llmcc::test::Expect(new_inode.content_digest != replaced.content_digest,
                      "inode replacement invalidates preserved-time memo");

  const fs::path outside = root / "outside";
  fs::create_directories(outside);
  const fs::path unsafe_base = root / "unsafe-base";
  fs::create_directories(unsafe_base);
  fs::create_directory_symlink(outside, unsafe_base / "model-digests");
  setenv("LLM_CC_ENTROPY_CACHE_DIR", unsafe_base.c_str(), 1);
  const auto symlink_fallback = llmcc::InspectModel(model, "abi", "cpu", 32);
  llmcc::test::ExpectEq(symlink_fallback.content_digest,
                        new_inode.content_digest,
                        "unsafe memo directory falls back to hashing");
  llmcc::test::Expect(fs::is_empty(outside),
                      "memo write does not follow digest directory symlink");

  const fs::path blocked_base = root / "blocked-base";
  Write(blocked_base, "file");
  setenv("LLM_CC_ENTROPY_CACHE_DIR", blocked_base.c_str(), 1);
  const auto unavailable = llmcc::InspectModel(model, "abi", "cpu", 32);
  llmcc::test::ExpectEq(unavailable.content_digest, new_inode.content_digest,
                        "unavailable memo storage falls back to hashing");

#if !defined(_WIN32)
  const fs::path racing = root / "racing.gguf";
  Write(racing, std::string(8 * 1024 * 1024, 'a'));
  setenv("LLM_CC_ENTROPY_CACHE_DIR", root.c_str(), 1);
  int ready[2]{};
  llmcc::test::Expect(pipe(ready) == 0, "create mutation handshake pipe");
  const pid_t writer = fork();
  llmcc::test::Expect(writer >= 0, "fork model mutator");
  if (writer == 0) {
    close(ready[0]);
    const int descriptor = open(racing.c_str(), O_WRONLY);
    const char started = 's';
    static_cast<void>(write(ready[1], &started, 1));
    if (descriptor < 0) _exit(1);
    for (std::uint64_t value = 0;; ++value) {
      const char byte = static_cast<char>('a' + value % 26);
      static_cast<void>(
          pwrite(descriptor, &byte, 1,
                 static_cast<off_t>((value * 4099) % (8 * 1024 * 1024))));
      static_cast<void>(fsync(descriptor));
    }
    _exit(1);
  }
  close(ready[1]);
  char started = '\0';
  llmcc::test::Expect(read(ready[0], &started, 1) == 1 && started == 's',
                      "mutator began before hashing");
  bool unstable = false;
  try {
    static_cast<void>(llmcc::InspectModel(racing, "abi", "cpu", 32));
  } catch (const std::runtime_error&) {
    unstable = true;
  }
  kill(writer, SIGTERM);
  int writer_status = 0;
  llmcc::test::Expect(waitpid(writer, &writer_status, 0) == writer,
                      "reap model mutator");
  llmcc::test::Expect(unstable,
                      "continuously changing model is rejected after retry");
#endif
  return 0;
}
