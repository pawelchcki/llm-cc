#include "src/backend_fetch.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "src/progress.h"
#include "src/sha256.h"

#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "src/download.h"

namespace fs = std::filesystem;

namespace llmcc {
namespace {

constexpr std::size_t kFooterSize = 64;
constexpr std::size_t kNameSize = 16;
constexpr std::size_t kSha256Size = 32;
constexpr std::size_t kHashBufferSize = 64 * 1024;

fs::path ChecksumPath(const fs::path& bundle) {
  auto checksum = bundle;
  checksum += fs::path(".sha256");
  return checksum;
}

fs::path PartialPath(const fs::path& path) {
  auto partial = path;
  partial += fs::path(".partial");
  return partial;
}

std::array<unsigned char, kSha256Size> HashFileRange(const fs::path& path,
                                                     std::uint64_t length) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open backend bundle " + path.string());
  }
  Sha256 hash;
  std::array<char, kHashBufferSize> buffer{};
  std::uint64_t remaining = length;
  while (remaining > 0) {
    const std::size_t requested = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer.size(), remaining));
    input.read(buffer.data(), static_cast<std::streamsize>(requested));
    const std::streamsize count = input.gcount();
    if (count <= 0) {
      throw std::runtime_error("cannot read backend bundle " + path.string());
    }
    hash.Update(
        std::span<const char>(buffer.data(), static_cast<std::size_t>(count)));
    remaining -= static_cast<std::uint64_t>(count);
  }
  return hash.Finish();
}

std::uint64_t FileSize(const fs::path& path) {
  std::error_code error;
  const std::uintmax_t size = fs::file_size(path, error);
  if (error || size > std::numeric_limits<std::uint64_t>::max()) {
    throw std::runtime_error("cannot inspect backend bundle " + path.string() +
                             (error ? ": " + error.message() : ""));
  }
  return static_cast<std::uint64_t>(size);
}

std::array<unsigned char, kSha256Size> HashFile(const fs::path& path) {
  return HashFileRange(path, FileSize(path));
}

std::string Hex(std::span<const unsigned char> bytes) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result(bytes.size() * 2, '0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    result[index * 2] = digits[bytes[index] >> 4];
    result[(index * 2) + 1] = digits[bytes[index] & 0xf];
  }
  return result;
}

fs::path RunningExecutablePath() {
#ifdef __linux__
  return "/proc/self/exe";
#elif defined(_WIN32)
  std::vector<wchar_t> path(260);
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0) {
      throw std::system_error(static_cast<int>(GetLastError()),
                              std::system_category(),
                              "cannot resolve the running executable path");
    }
    if (length < path.size()) {
      return fs::path(std::wstring(path.data(), length));
    }
    path.resize(path.size() * 2);
  }
#elif defined(__APPLE__)
  std::uint32_t size = 1024;
  std::string path(size, '\0');
  if (_NSGetExecutablePath(path.data(), &size) != 0) {
    path.assign(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
      throw std::runtime_error("cannot resolve the running executable path");
    }
  }
  path.resize(std::char_traits<char>::length(path.c_str()));
  return path;
#else
  throw std::runtime_error(
      "unstamped backend caches are unsupported on this platform");
#endif
}

std::string RunningExecutableIdentity() {
  static const std::string identity = Hex(HashFile(RunningExecutablePath()));
  return identity;
}

int HexDigit(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

std::array<unsigned char, kSha256Size> ReadRecordedHash(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read backend checksum " + path.string());
  }
  std::string text((std::istreambuf_iterator<char>(input)), {});
  if (input.bad()) {
    throw std::runtime_error("cannot read backend checksum " + path.string());
  }
  std::size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  if (text.size() - begin < kSha256Size * 2) {
    throw std::runtime_error("invalid backend checksum file " + path.string());
  }
  std::array<unsigned char, kSha256Size> result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    const int high = HexDigit(text[begin + (index * 2)]);
    const int low = HexDigit(text[begin + (index * 2) + 1]);
    if (high < 0 || low < 0) {
      throw std::runtime_error("invalid backend checksum file " +
                               path.string());
    }
    result[index] = static_cast<unsigned char>((high << 4) | low);
  }
  if (begin + (kSha256Size * 2) < text.size() &&
      !std::isspace(
          static_cast<unsigned char>(text[begin + (kSha256Size * 2)]))) {
    throw std::runtime_error("invalid backend checksum file " + path.string());
  }
  return result;
}

template <typename Integer>
Integer ReadLittleEndian(std::span<const char> input, std::size_t offset) {
  Integer result = 0;
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    result |=
        static_cast<Integer>(static_cast<unsigned char>(input[offset + index]))
        << (index * 8);
  }
  return result;
}

struct BundleFooter {
  std::uint64_t body_size;
  std::array<unsigned char, kSha256Size> body_hash;
};

BundleFooter ReadBundleFooter(const fs::path& bundle,
                              std::string_view expected_name) {
  const std::uint64_t size = FileSize(bundle);
  if (size < kFooterSize) {
    throw std::runtime_error("backend bundle footer is truncated");
  }
  const std::uint64_t body_size = size - kFooterSize;
  std::ifstream input(bundle, std::ios::binary);
  input.seekg(static_cast<std::streamoff>(body_size));
  std::array<char, kFooterSize> footer{};
  input.read(footer.data(), static_cast<std::streamsize>(footer.size()));
  if (!input) {
    throw std::runtime_error("cannot read backend bundle footer");
  }
  const std::size_t name_size = strnlen(footer.data(), kNameSize);
  if (name_size == kNameSize ||
      std::string_view(footer.data(), name_size) != expected_name ||
      !std::all_of(footer.begin() + name_size, footer.begin() + kNameSize,
                   [](char value) { return value == '\0'; })) {
    throw std::runtime_error("backend bundle footer name mismatch");
  }
  const std::uint64_t offset = ReadLittleEndian<std::uint64_t>(footer, 16);
  const std::uint64_t length = ReadLittleEndian<std::uint64_t>(footer, 24);
  if (offset != 0 || length != body_size) {
    throw std::runtime_error("backend bundle footer range is invalid");
  }
  std::array<unsigned char, kSha256Size> recorded{};
  std::memcpy(recorded.data(), footer.data() + 32, recorded.size());
  return {.body_size = body_size, .body_hash = recorded};
}

struct BundleHashes {
  std::array<unsigned char, kSha256Size> whole;
  std::array<unsigned char, kSha256Size> body;
};

BundleHashes HashBundle(const fs::path& path, std::uint64_t body_size) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open backend bundle " + path.string());
  }
  Sha256 whole;
  Sha256 body;
  std::array<char, kHashBufferSize> buffer{};
  const std::uint64_t total_size = body_size + kFooterSize;
  std::uint64_t completed = 0;
  while (completed < total_size) {
    const std::size_t requested = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer.size(), total_size - completed));
    input.read(buffer.data(), static_cast<std::streamsize>(requested));
    const std::streamsize count = input.gcount();
    if (count <= 0) {
      throw std::runtime_error("cannot read backend bundle " + path.string());
    }
    const std::span<const char> bytes(buffer.data(),
                                      static_cast<std::size_t>(count));
    whole.Update(bytes);
    if (completed < body_size) {
      const std::size_t body_count = static_cast<std::size_t>(
          std::min<std::uint64_t>(bytes.size(), body_size - completed));
      body.Update(bytes.first(body_count));
    }
    completed += static_cast<std::uint64_t>(count);
  }
  return {.whole = whole.Finish(), .body = body.Finish()};
}

void ValidateComponent(std::string_view value, std::string_view description) {
  if (value.empty() || value == "." || value == ".." ||
      value.find('\0') != std::string_view::npos ||
      value.find('/') != std::string_view::npos ||
      value.find('\\') != std::string_view::npos) {
    throw std::invalid_argument("invalid backend " + std::string(description));
  }
}

void ValidateOptions(const BackendFetchOptions& options) {
  if (options.name != "cuda" && options.name != "rocm") {
    throw std::invalid_argument("backend name must be cuda or rocm");
  }
  ValidateComponent(options.version, "version");
  ValidateComponent(options.configuration, "configuration");
  if (options.runtime_root.empty() ||
      options.runtime_root.native().find('\0') != std::string::npos) {
    throw std::invalid_argument("invalid backend runtime root");
  }
}

void CheckNotSymlink(const fs::path& path) {
  std::error_code error;
  const fs::file_status status = fs::symlink_status(path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("cannot inspect backend cache path " +
                             path.string() + ": " + error.message());
  }
  if (!error && fs::is_symlink(status)) {
    throw std::runtime_error("refusing to follow backend cache symlink " +
                             path.string());
  }
#if defined(_WIN32)
  if (!error) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      throw std::runtime_error(
          "refusing to follow backend cache reparse point " + path.string());
    }
  }
#endif
}

void EnsureCacheDirectory(const BackendFetchOptions& options,
                          const fs::path& version) {
  const fs::path backends = options.runtime_root / "backends";
  CheckNotSymlink(backends);
  CheckNotSymlink(version);
  std::error_code error;
  fs::create_directories(version, error);
  if (error) {
    throw std::runtime_error("cannot create backend cache " + version.string() +
                             ": " + error.message());
  }
  CheckNotSymlink(backends);
  CheckNotSymlink(version);
}

class BackendCacheLock {
 public:
  explicit BackendCacheLock(const fs::path& path) {
#if defined(__linux__) || defined(__APPLE__)
    descriptor_ =
        open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor_ < 0) {
      throw std::runtime_error("cannot open backend cache lock " +
                               path.string() + ": " + std::strerror(errno));
    }
    while (flock(descriptor_, LOCK_EX) != 0) {
      if (errno != EINTR) {
        const std::string message = "cannot acquire backend cache lock " +
                                    path.string() + ": " + std::strerror(errno);
        close(descriptor_);
        descriptor_ = -1;
        throw std::runtime_error(message);
      }
    }
#elif defined(_WIN32)
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      throw std::system_error(static_cast<int>(GetLastError()),
                              std::system_category(),
                              "cannot open backend cache lock");
    }
    if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlapped_)) {
      const DWORD error = GetLastError();
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      throw std::system_error(static_cast<int>(error), std::system_category(),
                              "cannot acquire backend cache lock");
    }
#else
    static_cast<void>(path);
    lock_ = std::unique_lock<std::mutex>(FallbackMutex());
#endif
  }

  ~BackendCacheLock() {
#if defined(__linux__) || defined(__APPLE__)
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
#elif defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) {
      UnlockFileEx(handle_, 0, 1, 0, &overlapped_);
      CloseHandle(handle_);
    }
#endif
  }

  BackendCacheLock(const BackendCacheLock&) = delete;
  BackendCacheLock& operator=(const BackendCacheLock&) = delete;

 private:
#if defined(__linux__) || defined(__APPLE__)
  int descriptor_ = -1;
#elif defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  OVERLAPPED overlapped_{};
#else
  static std::mutex& FallbackMutex() {
    static std::mutex mutex;
    return mutex;
  }
  std::unique_lock<std::mutex> lock_;
#endif
};

void RemoveFile(const fs::path& path) {
  std::error_code ignored;
  fs::remove(path, ignored);
}

std::string JoinUrl(std::string_view base, std::string_view file) {
  std::string result(base);
  if (!result.empty() && result.back() != '/') {
    result.push_back('/');
  }
  result.append(file);
  return result;
}

std::string UrlParent(std::string_view url) {
  const std::size_t slash = url.rfind('/');
  if (slash == std::string_view::npos) {
    return {};
  }
  return std::string(url.substr(0, slash));
}

std::string ExplicitArtifactName(std::string_view url) {
  const std::size_t slash = url.rfind('/');
  const std::string_view file =
      slash == std::string_view::npos ? url : url.substr(slash + 1);
  constexpr std::string_view suffix = ".bundle";
  if (!file.ends_with(suffix) || file.size() == suffix.size()) {
    throw std::invalid_argument("backend --url must identify a .bundle file");
  }
  return std::string(file.substr(0, file.size() - suffix.size()));
}

bool CacheIsValid(const BackendFetchOptions& options) {
  try {
    VerifyBackendBundle(options);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

std::optional<fs::path> DownloadManifest(const BundleDownloader& download,
                                         std::string_view base,
                                         std::string_view artifact,
                                         const fs::path& destination,
                                         bool required) {
  if (base.empty()) {
    return std::nullopt;
  }
  const std::array files = {std::string(artifact) + ".manifest.json",
                            std::string("manifest.json")};
  std::string failures;
  for (std::size_t index = 0; index < files.size(); ++index) {
    RemoveFile(destination);
    RemoveFile(PartialPath(destination));
    try {
      download(JoinUrl(base, files[index]), destination,
               {.noun = "backend bundle",
                .show_progress = index != 0,
                .record_in_model_manifest = false});
      return destination;
    } catch (const std::exception& error) {
      failures += std::string(error.what()) + "; ";
      RemoveFile(destination);
      RemoveFile(PartialPath(destination));
    }
  }
  if (required)
    throw std::runtime_error("backend manifest download failed: " + failures);
  return std::nullopt;
}

void VerifyManifest(const fs::path& manifest,
                    const BackendFetchOptions& options) {
  std::ifstream input(manifest);
  nlohmann::json contents;
  try {
    input >> contents;
  } catch (const nlohmann::json::exception& error) {
    throw std::runtime_error("invalid backend manifest: " +
                             std::string(error.what()));
  }
  if (!input || !contents.is_object() || !contents.contains("git_sha") ||
      !contents["git_sha"].is_string()) {
    throw std::runtime_error("invalid backend manifest: missing git_sha");
  }
  for (const auto& [field, expected] :
       {std::pair<std::string_view, std::string_view>{"version",
                                                      options.version},
        {"configuration", options.configuration},
        {"name", options.name}}) {
    if (!contents.contains(field) || !contents[field].is_string() ||
        contents[field].get<std::string>() != expected) {
      throw std::runtime_error("backend manifest " + std::string(field) +
                               " does not match this build");
    }
  }
  const std::string actual_sha = contents["git_sha"].get<std::string>();
  if (actual_sha != options.git_sha) {
    throw std::runtime_error(
        "backend bundle was built from a different commit: expected " +
        std::string(options.git_sha) + ", got " + actual_sha);
  }
}

}  // namespace

std::optional<std::string> BackendArtifactName(std::string_view name) {
#if defined(__linux__) && defined(__x86_64__)
  return "llm-cc-backend-" + std::string(name) + "-linux-x86_64";
#else
  static_cast<void>(name);
  return std::nullopt;
#endif
}

fs::path BackendBundlePath(const BackendFetchOptions& options) {
  ValidateOptions(options);
  std::string cache_version(options.version);
  if (options.git_sha.empty()) {
    const std::string identity = options.build_identity.empty()
                                     ? RunningExecutableIdentity()
                                     : std::string(options.build_identity);
    ValidateComponent(identity, "build identity");
    cache_version += ".build." + identity;
  } else {
    ValidateComponent(options.git_sha, "commit");
    cache_version += ".commit." + std::string(options.git_sha);
  }
  cache_version += ".config." + std::string(options.configuration);
  return options.runtime_root / "backends" / cache_version /
         (std::string(options.name) + ".bundle");
}

void VerifyBackendBundle(const BackendFetchOptions& options) {
  VerifyBackendBundle(options, BackendBundlePath(options));
}

void VerifyBackendBundle(const BackendFetchOptions& options,
                         const fs::path& bundle) {
  ValidateOptions(options);
  ReportPhase("verifying backend checksum and build commit");
  const fs::path checksum = ChecksumPath(bundle);
  fs::path manifest = bundle;
  manifest.replace_extension(".manifest.json");
  if (!fs::exists(manifest) &&
      bundle.filename() != std::string(options.name) + ".bundle") {
    manifest = bundle.parent_path() / "manifest.json";
  }
  CheckNotSymlink(bundle);
  CheckNotSymlink(checksum);
  CheckNotSymlink(manifest);
  if (!fs::is_regular_file(bundle) || !fs::is_regular_file(checksum)) {
    throw std::runtime_error(
        "backend bundle and checksum must both be regular files");
  }
  const BundleFooter footer = ReadBundleFooter(bundle, options.name);
  const BundleHashes hashes = HashBundle(bundle, footer.body_size);
  if (hashes.whole != ReadRecordedHash(checksum)) {
    throw std::runtime_error("backend bundle SHA-256 mismatch");
  }
  if (hashes.body != footer.body_hash) {
    throw std::runtime_error("backend bundle footer SHA-256 mismatch");
  }
  {
    if (!fs::is_regular_file(manifest)) {
      throw std::runtime_error(
          "backend manifest is required for build verification");
    }
    VerifyManifest(manifest, options);
  }
}

fs::path FetchBackendBundle(const BackendFetchOptions& options,
                            const BundleDownloader& downloader) {
  const fs::path bundle = BackendBundlePath(options);
  const fs::path checksum = ChecksumPath(bundle);
  const fs::path manifest =
      bundle.parent_path() / (std::string(options.name) + ".manifest.json");
  CheckNotSymlink(options.runtime_root / "backends");
  CheckNotSymlink(bundle.parent_path());
  if (CacheIsValid(options)) {
    return bundle;
  }

  CheckDownloadAllowed();
  const bool has_explicit_url =
      options.explicit_url.has_value() && !options.explicit_url->empty();
  if (!has_explicit_url && options.base_url.empty()) {
    throw std::runtime_error(
        "this build has no artifact base URL; --url must be used to fetch a "
        "backend bundle");
  }
  const std::optional<std::string> automatic_artifact =
      BackendArtifactName(options.name);
  if (!has_explicit_url && !automatic_artifact.has_value()) {
    throw std::runtime_error(
        "automatic backend fetching is unavailable for this target platform; "
        "use --url with a compatible bundle");
  }
  const std::string artifact = has_explicit_url
                                   ? ExplicitArtifactName(*options.explicit_url)
                                   : *automatic_artifact;
  EnsureCacheDirectory(options, bundle.parent_path());
  for (const fs::path& path : {bundle, checksum, manifest}) {
    CheckNotSymlink(path);
    CheckNotSymlink(PartialPath(path));
  }
  ReportPhase("waiting for backend cache lock");
  BackendCacheLock cache_lock(bundle.parent_path() /
                              ("." + std::string(options.name) + ".lock"));
  if (CacheIsValid(options)) {
    return bundle;
  }
  const std::string bundle_url =
      has_explicit_url ? *options.explicit_url
                       : JoinUrl(options.base_url, artifact + ".bundle");
  ConfirmDownload(bundle_url, bundle,
                  "backend bundle (includes checksum " + bundle_url +
                      ".sha256 and manifest from " + UrlParent(bundle_url) +
                      ")");
  RemoveFile(bundle);
  const std::string checksum_url = bundle_url + ".sha256";
  const std::string manifest_base =
      has_explicit_url ? UrlParent(bundle_url) : std::string(options.base_url);
  const BundleDownloader download = downloader ? downloader : DownloadFile;
  const DownloadOptions download_options{
      .noun = "backend bundle",
      .show_progress = true,
      .record_in_model_manifest = false,
  };

  try {
    download(bundle_url, bundle, download_options);
    download(checksum_url, checksum, download_options);
    const std::optional<fs::path> downloaded_manifest =
        DownloadManifest(download, manifest_base, artifact, manifest, true);
    {
      if (!downloaded_manifest.has_value()) {
        throw std::runtime_error(
            "backend manifest is required for build verification");
      }
    }
    VerifyBackendBundle(options);
  } catch (const std::exception& error) {
    RemoveFile(bundle);
    throw std::runtime_error(std::string(error.what()) + "; backend artifact " +
                             artifact + " for build commit " +
                             std::string(options.git_sha) +
                             "; publication may be unavailable. Recover with "
                             "'llm-cc backends fetch " +
                             std::string(options.name) +
                             " --url URL --assume-yes', --backend-dir DIR (or "
                             "LLM_CC_BACKEND_DIR), or --force-cpu");
  }
  return bundle;
}

}  // namespace llmcc
