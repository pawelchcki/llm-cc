#ifndef LLM_CC_BACKEND_FETCH_H_
#define LLM_CC_BACKEND_FETCH_H_

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "src/build_info.h"
#include "src/download.h"
#include "src/payload.h"

namespace llmcc {

struct BackendFetchOptions {
  std::string_view name;
  std::string_view version = build_info::Version();
  std::string_view git_sha = build_info::GitSha();
  std::string_view build_identity;
  std::string_view configuration = build_info::BackendConfiguration();
  std::string_view base_url = build_info::ArtifactBaseUrl();
  std::optional<std::string> explicit_url;
  std::filesystem::path runtime_root = RuntimeRoot();
};

using BundleDownloader = std::function<void(std::string_view url,
                                            const std::filesystem::path& target,
                                            const DownloadOptions& options)>;

std::optional<std::string> BackendArtifactName(std::string_view name);
// SHA-256 of the running executable: the identity of a build without a
// stamped commit.
std::string RunningExecutableIdentity();
std::filesystem::path BackendBundlePath(const BackendFetchOptions& options);
void VerifyBackendBundle(const BackendFetchOptions& options);
void VerifyBackendBundle(const BackendFetchOptions& options,
                         const std::filesystem::path& bundle);
std::filesystem::path FetchBackendBundle(
    const BackendFetchOptions& options,
    const BundleDownloader& downloader = {});

}  // namespace llmcc

#endif  // LLM_CC_BACKEND_FETCH_H_
