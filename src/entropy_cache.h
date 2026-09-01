#ifndef LLM_CC_ENTROPY_CACHE_H_
#define LLM_CC_ENTROPY_CACHE_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/jsonl.h"

namespace llmcc {

inline constexpr std::uint64_t kEntropyCacheLimit = 512ULL * 1024 * 1024;
inline constexpr std::uint64_t kEntropyCacheMaxAgeSeconds = 7ULL * 24 * 60 * 60;

struct ModelIdentity {
  std::filesystem::path canonical_path;
  std::uint64_t size;
  std::int64_t modification_time;
  std::string inference_abi;
  std::string backend;
  std::uint32_t context_limit;
};

struct EntropyCacheLookup {
  bool hit = false;
  std::vector<EntropyRecord> records;
};

struct RepositoryCacheStatus {
  std::filesystem::path repository;
  std::filesystem::path directory;
  std::uint64_t entries = 0;
  std::uint64_t bytes = 0;
};

ModelIdentity InspectModel(const std::filesystem::path& model,
                           std::string_view inference_abi,
                           std::string_view backend,
                           std::uint32_t context_limit);
std::string Sha256Hex(std::string_view contents);
std::string EntropyCacheKey(std::string_view preprocessed_source,
                            const ModelIdentity& model);
std::filesystem::path RepositoryCacheDirectory(
    const std::filesystem::path& repository);
EntropyCacheLookup ReadEntropyCache(const std::filesystem::path& repository,
                                    std::string_view preprocessed_source,
                                    const ModelIdentity& model);
void WriteEntropyCache(const std::filesystem::path& repository,
                       std::string_view preprocessed_source,
                       const ModelIdentity& model,
                       std::span<const EntropyRecord> records);
RepositoryCacheStatus GetRepositoryCacheStatus(
    const std::filesystem::path& repository);
void PruneRepositoryCache(const std::filesystem::path& repository,
                          bool force = true);
void ClearRepositoryCache(const std::filesystem::path& repository);

}  // namespace llmcc

#endif  // LLM_CC_ENTROPY_CACHE_H_
