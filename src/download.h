#ifndef LLM_CC_DOWNLOAD_H_
#define LLM_CC_DOWNLOAD_H_

#include <cstdint>
#include <filesystem>
#include <istream>
#include <optional>
#include <string_view>

namespace llmcc {

inline constexpr std::string_view kDefaultModelUrl =
    "https://huggingface.co/bartowski/DeepSeek-Coder-V2-Lite-Base-GGUF/"
    "resolve/main/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf";

void StreamDownload(std::istream& input, const std::filesystem::path& target,
                    std::uint64_t resume_offset,
                    std::optional<std::uint64_t> total_length);
void DownloadDefaultModel(const std::filesystem::path& target);

}  // namespace llmcc

#endif  // LLM_CC_DOWNLOAD_H_
