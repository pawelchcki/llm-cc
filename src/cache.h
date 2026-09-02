#ifndef LLM_CC_CACHE_H_
#define LLM_CC_CACHE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include "src/models.h"

namespace llmcc {

inline constexpr std::string_view kDefaultModelFile = kModels.front().file;

struct ModelTimestamps {
  std::optional<std::uint64_t> downloaded_at;
  std::optional<std::uint64_t> last_used_at;
};
using ModelManifest = std::map<std::string, ModelTimestamps>;

std::filesystem::path CacheDir();
std::filesystem::path CacheDirFrom(
    const std::optional<std::filesystem::path>& override_dir,
    const std::optional<std::filesystem::path>& xdg_cache_home,
    const std::optional<std::filesystem::path>& home);
std::filesystem::path PartialPath(const std::filesystem::path& target);
ModelManifest ReadManifest(const std::filesystem::path& cache_dir);
void MarkModelDownloaded(const std::filesystem::path& model);
void MarkCachedModelUsed(const std::filesystem::path& cache_dir,
                         const std::filesystem::path& model);
void ListModels(const std::filesystem::path& cache_dir, std::ostream& output);
void RemoveModel(const std::filesystem::path& cache_dir,
                 std::string_view file_name);
std::string FormatTimestamp(std::optional<std::uint64_t> timestamp);

std::filesystem::path ResolveModel(
    std::optional<std::filesystem::path> model, const ModelSpec& spec,
    bool no_download, const std::filesystem::path& current_dir,
    const std::filesystem::path& cache_dir,
    const std::function<void(std::string_view, const std::filesystem::path&)>&
        downloader);

std::filesystem::path ResolveModel(
    std::optional<std::filesystem::path> model, bool no_download,
    const std::filesystem::path& current_dir,
    const std::filesystem::path& cache_dir,
    const std::function<void(const std::filesystem::path&)>& downloader);

}  // namespace llmcc

#endif  // LLM_CC_CACHE_H_
