#include "src/entropy_cache.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "src/sha256.h"
#include "src/test_util.h"

namespace {
namespace fs = std::filesystem;

void Write(const fs::path& path, std::string_view value) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << value;
}

void WriteCbor(const fs::path& path, const nlohmann::json& value) {
  const auto bytes = nlohmann::json::to_cbor(value);
  Write(path, {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

llmcc::ModelIdentity Model() {
  return {.canonical_path = "unused.gguf",
          .size = 1,
          .modification_time = 1,
          .inference_abi = "test-abi",
          .backend = "cpu",
          .context_limit = 4096,
          .content_digest = "model-content-digest"};
}

std::vector<llmcc::EntropyRecord> Records(std::string_view source) {
  return {
      {.position = 0, .bytes = std::string(source), .entropy = std::nullopt}};
}

fs::path Entry(std::string_view source, const llmcc::ModelIdentity& model) {
  return llmcc::GlobalEntropyCacheDirectory() /
         (llmcc::EntropyCacheKey(source, model) + ".cbor");
}

std::uint64_t Size(std::string_view source, const llmcc::ModelIdentity& model) {
  return fs::file_size(Entry(source, model));
}

void ClearAndReset() {
  llmcc::SetEntropyCacheTestDeleteFailure(false);
  llmcc::SetEntropyCacheTestLimit(0);
  llmcc::ClearEntropyCache();
}
}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  const fs::path root = fs::path(std::getenv("TEST_TMPDIR")) / "entropy-cache";
  setenv("LLM_CC_ENTROPY_CACHE_DIR", root.c_str(), 1);
  const auto model = Model();
  llmcc::SetEntropyCacheTestNow(10'000'000);

  const auto records = Records("a");
  llmcc::WriteEntropyCache("a", model, records);
  llmcc::test::Expect(llmcc::ReadEntropyCache("a", model).hit,
                      "global v2 entry round trips");
  const auto status = llmcc::GetEntropyCacheStatus();
  llmcc::test::ExpectEq(status.entries, uint64_t{1}, "one global entry exists");
  llmcc::test::Expect(status.directory == root / "v2/entropy",
                      "global directory uses the v2 namespace");
  llmcc::test::ExpectEq(status.entries_by_inference_abi.at("test-abi"),
                        uint64_t{1}, "status reports inference provenance");

  auto changed = model;
  changed.content_digest = "replacement-content-digest";
  llmcc::test::Expect(!llmcc::ReadEntropyCache("a", changed).hit,
                      "model content invalidates entries");
  for (auto mutation : {0, 1, 2, 3, 4}) {
    auto different = model;
    if (mutation == 0) ++different.context_limit;
    if (mutation == 1) different.inference_abi = "other-abi";
    if (mutation == 2) different.backend = "metal";
    if (mutation == 3) ++different.batch_size;
    if (mutation == 4) different.reduction_policy = "host";
    llmcc::test::Expect(!llmcc::ReadEntropyCache("a", different).hit,
                        "every inference setting separates entries");
  }

  const auto entry = Entry("a", model);
  Write(entry, "corrupt");
  llmcc::test::Expect(!llmcc::ReadEntropyCache("a", model).hit,
                      "malformed CBOR is a miss");
  llmcc::WriteEntropyCache("a", model, records);
  WriteCbor(entry, {{"version", 2},
                    {"source_size", 1},
                    {"provenance", nlohmann::json::object()},
                    {"records", nlohmann::json::array()}});
  llmcc::test::Expect(!llmcc::ReadEntropyCache("a", model).hit,
                      "mismatched provenance is a miss");
  llmcc::WriteEntropyCache("a", model, records);
  WriteCbor(entry, {{"version", 2},
                    {"source_size", 1},
                    {"provenance",
                     {{"source_sha256", llmcc::Sha256Hex("a")},
                      {"model_sha256", model.content_digest},
                      {"inference_abi", model.inference_abi},
                      {"backend", model.backend},
                      {"context_limit", model.context_limit},
                      {"batch_size", model.batch_size},
                      {"reduction_policy", model.reduction_policy},
                      {"effective_reducer", model.effective_reducer}}},
                    {"records", nlohmann::json::array()}});
  llmcc::test::Expect(!llmcc::ReadEntropyCache("a", model).hit,
                      "incomplete token coverage is a miss");

  ClearAndReset();
  llmcc::SetEntropyCacheTestNow(20'000'000);
  llmcc::WriteEntropyCache("a", model, records);
  llmcc::SetEntropyCacheTestNow(20'000'000 + llmcc::kEntropyCacheMaxAgeSeconds);
  llmcc::test::Expect(!llmcc::ReadEntropyCache("a", model).hit,
                      "entry expires at exactly twenty days before touch");

  ClearAndReset();
  llmcc::SetEntropyCacheTestNow(30'000'000);
  llmcc::WriteEntropyCache("a", model, records);
  const auto single_size = Size("a", model);
  llmcc::SetEntropyCacheTestNow(30'000'010);
  llmcc::WriteEntropyCache("b", model, Records("b"));
  llmcc::SetEntropyCacheTestNow(30'000'020);
  llmcc::test::Expect(llmcc::ReadEntropyCache("a", model).hit,
                      "reading an unexpired entry refreshes recency");
  llmcc::SetEntropyCacheTestLimit(single_size * 2);
  llmcc::SetEntropyCacheTestNow(30'000'030);
  llmcc::WriteEntropyCache("c", model, Records("c"));
  llmcc::test::Expect(llmcc::ReadEntropyCache("a", model).hit,
                      "recently refreshed entry survives LRU eviction");
  llmcc::test::Expect(!llmcc::ReadEntropyCache("b", model).hit,
                      "least recently used entry is evicted first");

  ClearAndReset();
  llmcc::SetEntropyCacheTestLimit(1);
  llmcc::WriteEntropyCache("a", model, records);
  llmcc::test::ExpectEq(llmcc::GetEntropyCacheStatus().entries, uint64_t{0},
                        "oversized entry is not committed");

  llmcc::SetEntropyCacheTestLimit(0);
  llmcc::WriteEntropyCache("a", model, records);
  const auto one_size = Size("a", model);
  llmcc::WriteEntropyCache("b", model, Records("b"));
  llmcc::SetEntropyCacheTestLimit(one_size * 2);
  llmcc::SetEntropyCacheTestDeleteFailure(true);
  llmcc::WriteEntropyCache("c", model, Records("c"));
  llmcc::test::Expect(!llmcc::ReadEntropyCache("c", model).hit,
                      "failed eviction skips publication");
  llmcc::SetEntropyCacheTestDeleteFailure(false);

  Write(root / "v2/accounting.json", "interrupted accounting");
  llmcc::WriteEntropyCache("d", model, Records("d"));
  llmcc::test::Expect(llmcc::ReadEntropyCache("d", model).hit,
                      "malformed accounting is rebuilt");
  // Simulate death after marking accounting dirty but before recording the
  // committed files. A new write must reconstruct sizes rather than trust 0.
  Write(root / "v2/accounting.json",
        R"({"bytes":0,"entries":0,"next_expiry":0,"dirty":true})");
  llmcc::WriteEntropyCache("e", model, Records("e"));
  const auto recovered = llmcc::GetEntropyCacheStatus();
  llmcc::test::Expect(recovered.bytes <= one_size * 2 && recovered.entries == 2,
                      "dirty accounting rebuild preserves the byte budget");
  const auto temporary =
      llmcc::GlobalEntropyCacheDirectory() / "abandoned.cbor.tmp.test";
  const auto metadata_temporary = root / "v2/accounting.json.tmp.abandoned";
  Write(temporary, "temporary");
  Write(metadata_temporary, "metadata temporary");
  llmcc::PruneEntropyCache();
  llmcc::test::Expect(
      !fs::exists(temporary) && !fs::exists(metadata_temporary),
      "pruning removes abandoned entropy and metadata temporaries");

  llmcc::SetEntropyCacheTestLimit(0);
  llmcc::WriteEntropyCache("d", model, Records("d"));

  const auto accounting = root / "v2/accounting.json";
  fs::remove(accounting);

  const auto permanent_lock = root / "v2/.lock";
  fs::remove(permanent_lock);
  fs::create_directory(permanent_lock);
  llmcc::test::Expect(
      llmcc::ReadEntropyCache("d", model).hit,
      "validated hit survives lock and timestamp refresh failure");
  fs::remove(permanent_lock);
  fs::create_directory(accounting);
  llmcc::SetEntropyCacheTestNow(30'000'030 + 86400);
  llmcc::test::Expect(llmcc::ReadEntropyCache("d", model).hit,
                      "accounting maintenance failure preserves a valid hit");
  fs::remove(accounting);

  const auto old_entry = llmcc::RepositoryCacheDirectory(root / "old") /
                         (llmcc::EntropyCacheKey("legacy", model) + ".cbor");
  Write(old_entry, "old-v1-entry");
  llmcc::test::Expect(!llmcc::ReadEntropyCache("legacy", model).hit,
                      "v1 repository cache is not promoted to v2");

#if !defined(_WIN32)
  const auto public_permissions = fs::perms::group_all | fs::perms::others_all;
  llmcc::test::Expect((fs::status(root / "v2").permissions() &
                       public_permissions) == fs::perms::none,
                      "v2 namespace is private");
  const fs::path unsafe = root / "unsafe";
  fs::create_directories(unsafe);
  fs::create_directory_symlink(root / "elsewhere", unsafe / "v2");
  setenv("LLM_CC_ENTROPY_CACHE_DIR", unsafe.c_str(), 1);
  bool refused = false;
  try {
    llmcc::CheckEntropyCacheAvailability();
  } catch (const std::exception&) {
    refused = true;
  }
  llmcc::test::Expect(refused, "cache availability refuses directory symlinks");
#endif
  return 0;
}
