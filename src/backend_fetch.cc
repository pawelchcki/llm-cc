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

constexpr std::array<std::uint32_t, 64> kShaConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

std::uint32_t RotateRight(std::uint32_t value, unsigned int bits) {
  return (value >> bits) | (value << (32 - bits));
}

class Sha256 {
 public:
  void Update(std::span<const char> input) {
    constexpr std::uint64_t kMaximumBytes =
        std::numeric_limits<std::uint64_t>::max() / 8;
    if (input.size() > kMaximumBytes - bytes_) {
      throw std::runtime_error("backend bundle is too large to hash");
    }
    bytes_ += input.size();
    for (char byte : input) {
      block_[block_size_++] = static_cast<unsigned char>(byte);
      if (block_size_ == block_.size()) {
        Transform();
        block_size_ = 0;
      }
    }
  }

  std::array<unsigned char, kSha256Size> Finish() {
    const std::uint64_t bit_length = bytes_ * 8;
    block_[block_size_++] = 0x80;
    if (block_size_ > 56) {
      std::fill(block_.begin() + block_size_, block_.end(), 0);
      Transform();
      block_size_ = 0;
    }
    std::fill(block_.begin() + block_size_, block_.begin() + 56, 0);
    for (std::size_t index = 0; index < 8; ++index) {
      block_[63 - index] =
          static_cast<unsigned char>(bit_length >> (index * 8));
    }
    Transform();

    std::array<unsigned char, kSha256Size> result{};
    for (std::size_t word = 0; word < hash_.size(); ++word) {
      for (std::size_t byte = 0; byte < 4; ++byte) {
        result[(word * 4) + byte] =
            static_cast<unsigned char>(hash_[word] >> ((3 - byte) * 8));
      }
    }
    return result;
  }

 private:
  void Transform() {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      const std::size_t offset = index * 4;
      words[index] = (static_cast<std::uint32_t>(block_[offset]) << 24) |
                     (static_cast<std::uint32_t>(block_[offset + 1]) << 16) |
                     (static_cast<std::uint32_t>(block_[offset + 2]) << 8) |
                     block_[offset + 3];
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const std::uint32_t s0 = RotateRight(words[index - 15], 7) ^
                               RotateRight(words[index - 15], 18) ^
                               (words[index - 15] >> 3);
      const std::uint32_t s1 = RotateRight(words[index - 2], 17) ^
                               RotateRight(words[index - 2], 19) ^
                               (words[index - 2] >> 10);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = hash_;
    for (std::size_t index = 0; index < words.size(); ++index) {
      const std::uint32_t sum1 =
          RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
      const std::uint32_t choose = (e & f) ^ ((~e) & g);
      const std::uint32_t temporary1 =
          h + sum1 + choose + kShaConstants[index] + words[index];
      const std::uint32_t sum0 =
          RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temporary2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    hash_[0] += a;
    hash_[1] += b;
    hash_[2] += c;
    hash_[3] += d;
    hash_[4] += e;
    hash_[5] += f;
    hash_[6] += g;
    hash_[7] += h;
  }

  std::array<std::uint32_t, 8> hash_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                        0xa54ff53a, 0x510e527f, 0x9b05688c,
                                        0x1f83d9ab, 0x5be0cd19};
  std::array<unsigned char, 64> block_{};
  std::size_t block_size_ = 0;
  std::uint64_t bytes_ = 0;
};

std::array<unsigned char, kSha256Size> HashFileRange(const fs::path& path,
                                                     std::uint64_t length) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open backend bundle " + path.string());
  }
  Sha256 hash;
  std::array<char, std::size_t{1024} * 1024> buffer{};
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
  std::array<char, std::size_t{1024} * 1024> buffer{};
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
#endif
  }

  BackendCacheLock(const BackendCacheLock&) = delete;
  BackendCacheLock& operator=(const BackendCacheLock&) = delete;

 private:
#if defined(__linux__) || defined(__APPLE__)
  int descriptor_ = -1;
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
                                         const fs::path& destination) {
  if (base.empty()) {
    return std::nullopt;
  }
  const std::array files = {std::string(artifact) + ".manifest.json",
                            std::string("manifest.json")};
  for (std::size_t index = 0; index < files.size(); ++index) {
    RemoveFile(destination);
    RemoveFile(destination.string() + ".partial");
    try {
      download(JoinUrl(base, files[index]), destination,
               {.noun = "backend bundle",
                .show_progress = index != 0,
                .record_in_model_manifest = false});
      return destination;
    } catch (const std::exception&) {
      RemoveFile(destination);
      RemoveFile(destination.string() + ".partial");
    }
  }
  return std::nullopt;
}

void VerifyManifest(const fs::path& manifest, std::string_view expected_sha) {
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
  const std::string actual_sha = contents["git_sha"].get<std::string>();
  if (actual_sha != expected_sha) {
    throw std::runtime_error(
        "backend bundle was built from a different commit: expected " +
        std::string(expected_sha) + ", got " + actual_sha);
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
  }
  return options.runtime_root / "backends" / cache_version /
         (std::string(options.name) + ".bundle");
}

void VerifyBackendBundle(const BackendFetchOptions& options) {
  const fs::path bundle = BackendBundlePath(options);
  const fs::path checksum = bundle.string() + ".sha256";
  const fs::path manifest =
      bundle.parent_path() / (std::string(options.name) + ".manifest.json");
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
  if (!options.git_sha.empty()) {
    if (!fs::is_regular_file(manifest)) {
      throw std::runtime_error(
          "backend manifest is required for a stamped build");
    }
    VerifyManifest(manifest, options.git_sha);
  }
}

fs::path FetchBackendBundle(const BackendFetchOptions& options,
                            const BundleDownloader& downloader) {
  const fs::path bundle = BackendBundlePath(options);
  const fs::path checksum = bundle.string() + ".sha256";
  const fs::path manifest =
      bundle.parent_path() / (std::string(options.name) + ".manifest.json");
  CheckNotSymlink(options.runtime_root / "backends");
  CheckNotSymlink(bundle.parent_path());
  if (CacheIsValid(options)) {
    return bundle;
  }

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
    CheckNotSymlink(path.string() + ".partial");
  }
  BackendCacheLock cache_lock(bundle.parent_path() /
                              ("." + std::string(options.name) + ".lock"));
  if (CacheIsValid(options)) {
    return bundle;
  }
  RemoveFile(bundle);

  const std::string bundle_url =
      has_explicit_url ? *options.explicit_url
                       : JoinUrl(options.base_url, artifact + ".bundle");
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
        DownloadManifest(download, manifest_base, artifact, manifest);
    if (!options.git_sha.empty()) {
      if (!downloaded_manifest.has_value()) {
        throw std::runtime_error(
            "backend manifest is required for a stamped build");
      }
    }
    VerifyBackendBundle(options);
  } catch (...) {
    RemoveFile(bundle);
    throw;
  }
  return bundle;
}

}  // namespace llmcc
