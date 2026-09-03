#ifndef LLM_CC_DOWNLOAD_H_
#define LLM_CC_DOWNLOAD_H_

#include <cstdint>
#include <filesystem>
#include <istream>
#include <optional>
#include <string_view>

#include "src/models.h"

namespace llmcc {

inline constexpr std::string_view kDefaultModelUrl = kModels.front().url;

struct DownloadOptions {
  std::string_view noun;
  bool show_progress = true;
  bool record_in_model_manifest = false;
};

void StreamDownload(std::istream& input, const std::filesystem::path& target,
                    std::uint64_t resume_offset,
                    std::optional<std::uint64_t> total_length);
void DownloadFile(std::string_view url, const std::filesystem::path& target,
                  const DownloadOptions& options);
void DownloadModel(std::string_view url, const std::filesystem::path& target);
void DownloadDefaultModel(const std::filesystem::path& target);

}  // namespace llmcc

#endif  // LLM_CC_DOWNLOAD_H_
