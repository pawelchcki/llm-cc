#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "src/entropy_cache.h"
#include "src/sha256.h"
#include "src/test_util.h"

namespace {

std::vector<llmcc::EntropyRecord> Records(std::string_view source) {
  return {{.position = 0, .bytes = std::string(source), .entropy = 0.5}};
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  namespace fs = std::filesystem;
  const char* temporary = std::getenv("TEST_TMPDIR");
  llmcc::test::Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const fs::path root = fs::path(temporary) / "concurrent-entropy";
  setenv("LLM_CC_ENTROPY_CACHE_DIR", root.c_str(), 1);
  const llmcc::ModelIdentity model{.canonical_path = "unused.gguf",
                                   .size = 1,
                                   .modification_time = 0,
                                   .inference_abi = "concurrency-test",
                                   .backend = "cpu",
                                   .context_limit = 4096,
                                   .content_digest = llmcc::Sha256Hex("model")};
  llmcc::WriteEntropyCache("initial", model, Records("initial"));
  const fs::path lock = root / "v2/.lock";
  const fs::path lock_reference = root / "lock-reference";
  fs::create_hard_link(lock, lock_reference);
  // Each child opens its own lock after fork. Writers overlap reads, sweeping,
  // and clearing, exercising the permanent lock through repeated empty stores.
  std::vector<pid_t> children;
  for (int role = 0; role < 6; ++role) {
    const pid_t child = fork();
    llmcc::test::Expect(child >= 0, "fork succeeds");
    if (child == 0) {
      try {
        for (int iteration = 0; iteration < 35; ++iteration) {
          if (role == 4) {
            llmcc::PruneEntropyCache();
          } else if (role == 5) {
            llmcc::ClearEntropyCache();
          } else {
            const std::string source = "source-" + std::to_string(role) + "-" +
                                       std::to_string(iteration % 7);
            llmcc::WriteEntropyCache(source, model, Records(source));
            const auto hit = llmcc::ReadEntropyCache(source, model);
            // A concurrent clear may turn a lookup into a miss; a hit must
            // always contain the exact complete source and valid entropy.
            if (hit.hit && (hit.records.size() != 1 ||
                            hit.records.front().bytes != source ||
                            hit.records.front().entropy != 0.5)) {
              _exit(2);
            }
          }
        }
        _exit(0);
      } catch (...) {
        _exit(3);
      }
    }
    children.push_back(child);
  }
  for (const auto child : children) {
    int status = 0;
    llmcc::test::Expect(waitpid(child, &status, 0) == child &&
                            WIFEXITED(status) && WEXITSTATUS(status) == 0,
                        "concurrent cache process completed successfully");
  }
  llmcc::test::Expect(fs::equivalent(lock, lock_reference),
                      "clear preserves the permanent lock inode");
  llmcc::PruneEntropyCache();
  std::uint64_t bytes = 0;
  std::uint64_t entries = 0;
  for (const auto& entry :
       fs::directory_iterator(llmcc::GlobalEntropyCacheDirectory())) {
    llmcc::test::Expect(entry.path().extension() == ".cbor",
                        "concurrent publication leaves no temporary entries");
    std::ifstream input(entry.path(), std::ios::binary);
    const std::vector<std::uint8_t> encoded{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    const auto decoded = nlohmann::json::from_cbor(encoded);
    llmcc::test::ExpectEq(decoded.at("version").get<int>(), 2,
                          "all concurrent entries are valid v2 CBOR");
    bytes += entry.file_size();
    ++entries;
  }
  std::ifstream accounting(root / "v2/accounting.json");
  const auto record = nlohmann::json::parse(accounting);
  llmcc::test::Expect(!record.at("dirty").get<bool>(),
                      "accounting is clean after concurrency");
  llmcc::test::ExpectEq(record.at("bytes").get<std::uint64_t>(), bytes,
                        "accounting matches actual committed bytes");
  llmcc::test::ExpectEq(record.at("entries").get<std::uint64_t>(), entries,
                        "accounting matches actual entry count");
  llmcc::test::Expect(bytes <= llmcc::kEntropyCacheLimit,
                      "concurrent publication respects byte budget");
}
