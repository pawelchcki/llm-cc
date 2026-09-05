#ifndef LLM_CC_ENTROPY_CACHE_H_
#define LLM_CC_ENTROPY_CACHE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/jsonl.h"
#include "src/model_identity.h"

namespace llmcc {

inline constexpr std::uint64_t kEntropyCacheLimit = 1024ULL * 1024 * 1024;
inline constexpr std::uint64_t kEntropyCacheMaxAgeSeconds =
    20ULL * 24 * 60 * 60;

struct EntropyCacheLookup {
  bool hit = false;
  std::vector<EntropyRecord> records;
};

struct RepositoryCacheStatus {
  std::filesystem::path repository;
  std::filesystem::path directory;
  std::filesystem::path legacy_directory;
  std::uint64_t entries = 0;
  std::uint64_t bytes = 0;
  std::uint64_t legacy_entries = 0;
  std::uint64_t legacy_bytes = 0;
  std::uint64_t unknown_provenance_entries = 0;
  std::uint64_t malformed_entries = 0;
  std::map<std::string, std::uint64_t> entries_by_inference_abi;
};

struct EntropyCacheStatus {
  std::filesystem::path directory;
  std::uint64_t entries = 0;
  std::uint64_t bytes = 0;
  std::uint64_t malformed_entries = 0;
  std::uint64_t limit = kEntropyCacheLimit;
  std::uint64_t retention_seconds = kEntropyCacheMaxAgeSeconds;
  unsigned storage_version = 2;
  std::map<std::string, std::uint64_t> entries_by_inference_abi;
};

std::string EntropyCacheKey(std::string_view preprocessed_source,
                            const ModelIdentity& model);
std::filesystem::path EntropyCacheBaseDirectory();
std::filesystem::path GlobalEntropyCacheDirectory();
void CheckEntropyCacheAvailability();
EntropyCacheLookup ReadEntropyCache(std::string_view preprocessed_source,
                                    const ModelIdentity& model);
void WriteEntropyCache(std::string_view preprocessed_source,
                       const ModelIdentity& model,
                       std::span<const EntropyRecord> records);
EntropyCacheStatus GetEntropyCacheStatus(bool inspect_provenance = true);
void PruneEntropyCache(bool force = true);
void ClearEntropyCache();

// Test seams.  A value of zero restores the system clock/default budget.
void SetEntropyCacheTestNow(std::uint64_t epoch_seconds);
void SetEntropyCacheTestLimit(std::uint64_t bytes);
void SetEntropyCacheTestDeleteFailure(bool enabled);
std::filesystem::path RepositoryCacheDirectory(
    const std::filesystem::path& repository);
std::filesystem::path LegacyRepositoryCacheDirectory(
    const std::filesystem::path& repository);
RepositoryCacheStatus GetRepositoryCacheStatus(
    const std::filesystem::path& repository);
void ClearRepositoryCache(const std::filesystem::path& repository);
void ClearLegacyRepositoryCache(const std::filesystem::path& repository);

}  // namespace llmcc

#endif  // LLM_CC_ENTROPY_CACHE_H_
