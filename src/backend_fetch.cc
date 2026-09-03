#include "src/backend_fetch.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

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

void VerifyFooter(const fs::path& bundle, std::string_view expected_name) {
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
  if (HashFileRange(bundle, body_size) != recorded) {
    throw std::runtime_error("backend bundle footer SHA-256 mismatch");
  }
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

void EnsureCacheDirectory(const BackendFetchOptions& options) {
  const fs::path backends = options.runtime_root / "backends";
  const fs::path version = backends / options.version;
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

void RemoveFile(const fs::path& path) {
  std::error_code ignored;
  fs::remove(path, ignored);
}

std::string ArtifactName(std::string_view name) {
  return "llm-cc-backend-" + std::string(name) + "-linux-x86_64";
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

bool CacheMatches(const fs::path& bundle, const fs::path& checksum) {
  try {
    CheckNotSymlink(bundle);
    CheckNotSymlink(checksum);
    return fs::is_regular_file(bundle) && fs::is_regular_file(checksum) &&
           HashFile(bundle) == ReadRecordedHash(checksum);
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

fs::path BackendBundlePath(const BackendFetchOptions& options) {
  ValidateOptions(options);
  return options.runtime_root / "backends" / options.version /
         (std::string(options.name) + ".bundle");
}

fs::path FetchBackendBundle(const BackendFetchOptions& options,
                            const BundleDownloader& downloader) {
  const fs::path bundle = BackendBundlePath(options);
  const fs::path checksum = bundle.string() + ".sha256";
  const fs::path manifest =
      bundle.parent_path() / (std::string(options.name) + ".manifest.json");
  CheckNotSymlink(options.runtime_root / "backends");
  CheckNotSymlink(bundle.parent_path());
  if (CacheMatches(bundle, checksum)) {
    return bundle;
  }

  const bool has_explicit_url =
      options.explicit_url.has_value() && !options.explicit_url->empty();
  if (!has_explicit_url && options.base_url.empty()) {
    throw std::runtime_error(
        "this build has no artifact base URL; --url must be used to fetch a "
        "backend bundle");
  }
  EnsureCacheDirectory(options);
  for (const fs::path& path : {bundle, checksum, manifest}) {
    CheckNotSymlink(path);
    CheckNotSymlink(path.string() + ".partial");
  }
  RemoveFile(bundle);

  const std::string artifact = ArtifactName(options.name);
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
    if (HashFile(bundle) != ReadRecordedHash(checksum)) {
      throw std::runtime_error("backend bundle SHA-256 mismatch");
    }
    VerifyFooter(bundle, options.name);
    const std::optional<fs::path> downloaded_manifest =
        DownloadManifest(download, manifest_base, artifact, manifest);
    if (!options.git_sha.empty()) {
      if (!downloaded_manifest.has_value()) {
        throw std::runtime_error(
            "backend manifest is required for a stamped build");
      }
      VerifyManifest(*downloaded_manifest, options.git_sha);
    }
  } catch (...) {
    RemoveFile(bundle);
    throw;
  }
  return bundle;
}

}  // namespace llmcc
