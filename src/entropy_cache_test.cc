#include "src/entropy_cache.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "src/test_util.h"

namespace {

void Write(const std::filesystem::path& path, std::string_view value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << value;
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  namespace fs = std::filesystem;
  const char* temporary = std::getenv("TEST_TMPDIR");
  llmcc::test::Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const fs::path repository = fs::path(temporary) / "repository";
  const fs::path model = fs::path(temporary) / "model.gguf";
  fs::create_directories(repository);
  Write(model, "model");
  const auto identity = llmcc::InspectModel(model, "test-abi", "cpu", 4096);
  const std::vector<llmcc::EntropyRecord> records = {
      {.position = 0, .bytes = "a", .entropy = std::nullopt},
      {.position = 1, .bytes = "b", .entropy = 0.5}};
  llmcc::WriteEntropyCache(repository, "ab", identity, records);
  auto lookup = llmcc::ReadEntropyCache(repository, "ab", identity);
  llmcc::test::Expect(lookup.hit, "CBOR cache round trip hits");
  llmcc::test::ExpectEq(lookup.records[1].entropy, std::optional<double>{0.5},
                        "entropy round trips");

  const auto initial_status = llmcc::GetRepositoryCacheStatus(repository);
  llmcc::test::ExpectEq(initial_status.entries, std::uint64_t{1},
                        "one cache entry exists");
  const fs::path entry = *fs::directory_iterator(initial_status.directory);
  fs::last_write_time(
      entry, fs::file_time_type::clock::now() - std::chrono::hours(48));
  static_cast<void>(llmcc::ReadEntropyCache(repository, "ab", identity));
  llmcc::test::Expect(
      fs::last_write_time(entry) >
          fs::file_time_type::clock::now() - std::chrono::hours(1),
      "cache hit touches entry for LRU");

  Write(entry, "corrupt");
  llmcc::test::Expect(!llmcc::ReadEntropyCache(repository, "ab", identity).hit,
                      "corruption is treated as a miss");
  llmcc::WriteEntropyCache(repository, "ab", identity, records);

  nlohmann::json invalid_entropy = {
      {"version", 1},
      {"source_size", 2},
      {"records",
       nlohmann::json::array(
           {nlohmann::json::array(
                {nlohmann::json::binary(std::vector<std::uint8_t>{'a'}),
                 nullptr}),
            nlohmann::json::array(
                {nlohmann::json::binary(std::vector<std::uint8_t>{'b'}),
                 nullptr})})}};
  const auto invalid_cbor = nlohmann::json::to_cbor(invalid_entropy);
  Write(entry, std::string(reinterpret_cast<const char*>(invalid_cbor.data()),
                           invalid_cbor.size()));
  llmcc::test::Expect(
      !llmcc::ReadEntropyCache(repository, "ab", identity).hit,
      "null entropy after the first token is treated as corruption");
  llmcc::WriteEntropyCache(repository, "ab", identity, records);

  auto changed_context = identity;
  changed_context.context_limit++;
  llmcc::test::Expect(
      !llmcc::ReadEntropyCache(repository, "ab", changed_context).hit,
      "context limit invalidates key");
  auto changed_backend = identity;
  changed_backend.backend = "metal";
  llmcc::test::Expect(
      !llmcc::ReadEntropyCache(repository, "ab", changed_backend).hit,
      "backend invalidates key");
  auto changed_batch = identity;
  changed_batch.batch_size++;
  llmcc::test::Expect(
      !llmcc::ReadEntropyCache(repository, "ab", changed_batch).hit,
      "batch size invalidates key");
  auto changed_reducer = identity;
  changed_reducer.reduction_policy = "host";
  llmcc::test::Expect(
      !llmcc::ReadEntropyCache(repository, "ab", changed_reducer).hit,
      "reduction policy invalidates key");
  llmcc::test::Expect(llmcc::EntropyCacheKey("ab", identity) !=
                          llmcc::EntropyCacheKey("ac", identity),
                      "source digest invalidates key");
  llmcc::test::ExpectEq(
      llmcc::Sha256Hex("abc"),
      std::string(
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
      "SHA-256 implementation");

  fs::last_write_time(
      entry, fs::file_time_type::clock::now() - std::chrono::hours(24 * 8));
  llmcc::PruneRepositoryCache(repository);
  llmcc::test::ExpectEq(llmcc::GetRepositoryCacheStatus(repository).entries,
                        std::uint64_t{0}, "seven-day expiry is pruned");

  llmcc::WriteEntropyCache(repository, "ab", identity, records);
  Write(repository / ".llm-cc-cache/other/keep", "keep");
  llmcc::ClearRepositoryCache(repository);
  llmcc::test::Expect(fs::exists(repository / ".llm-cc-cache/other/keep"),
                      "clear preserves other cache namespaces");
  llmcc::test::ExpectEq(llmcc::GetRepositoryCacheStatus(repository).entries,
                        std::uint64_t{0}, "clear removes llm-cc entries");

  const fs::path unsafe = fs::path(temporary) / "unsafe";
  fs::create_directories(unsafe);
  fs::create_directory_symlink(fs::path(temporary) / "elsewhere",
                               unsafe / ".llm-cc-cache");
  bool refused = false;
  try {
    llmcc::WriteEntropyCache(unsafe, "ab", identity, records);
  } catch (const std::runtime_error&) {
    refused = true;
  }
  llmcc::test::Expect(refused, "cache directory symlink is refused");
  return 0;
}
