#ifndef LLM_CC_BACKEND_FETCH_H_
#define LLM_CC_BACKEND_FETCH_H_

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "generated/version.h"
#include "src/download.h"
#include "src/payload.h"

namespace llmcc {

struct BackendFetchOptions {
  std::string_view name;
  std::string_view version = LLM_CC_VERSION;
  std::string_view git_sha{LLM_CC_GIT_SHA, sizeof(LLM_CC_GIT_SHA) - 1};
  std::string_view base_url{LLM_CC_ARTIFACT_BASE_URL,
                            sizeof(LLM_CC_ARTIFACT_BASE_URL) - 1};
  std::optional<std::string> explicit_url;
  std::filesystem::path runtime_root = RuntimeRoot();
};

using BundleDownloader = std::function<void(std::string_view url,
                                            const std::filesystem::path& target,
                                            const DownloadOptions& options)>;

std::filesystem::path BackendBundlePath(const BackendFetchOptions& options);
void VerifyBackendBundle(const BackendFetchOptions& options);
std::filesystem::path FetchBackendBundle(
    const BackendFetchOptions& options,
    const BundleDownloader& downloader = {});

}  // namespace llmcc

#endif  // LLM_CC_BACKEND_FETCH_H_
