#include "src/model_identity.h"

#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "src/cache_io.h"
#include "src/progress.h"
#include "src/sha256.h"

#if defined(_WIN32)
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif

#if !defined(_WIN32)
#include <sys/stat.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace llmcc {
std::filesystem::path EntropyCacheBaseDirectory();
namespace {

class NativeSha256 {
 public:
  NativeSha256() {
#if defined(_WIN32)
    if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) != 0) {
      throw std::runtime_error("cannot initialize SHA-256");
    }
    if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) != 0) {
      BCryptCloseAlgorithmProvider(algorithm_, 0);
      algorithm_ = nullptr;
      throw std::runtime_error("cannot initialize SHA-256");
    }
#elif defined(__APPLE__)
    if (CC_SHA256_Init(&context_) != 1) {
      throw std::runtime_error("cannot initialize SHA-256");
    }
#else
    context_ = EVP_MD_CTX_new();
    if (context_ == nullptr ||
        EVP_DigestInit_ex(context_, EVP_sha256(), nullptr) != 1) {
      EVP_MD_CTX_free(context_);
      context_ = nullptr;
      throw std::runtime_error("cannot initialize SHA-256");
    }
#endif
  }

  NativeSha256(const NativeSha256&) = delete;
  NativeSha256& operator=(const NativeSha256&) = delete;

  ~NativeSha256() {
#if defined(_WIN32)
    if (hash_ != nullptr) {
      BCryptDestroyHash(hash_);
    }
    if (algorithm_ != nullptr) {
      BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
#elif !defined(__APPLE__)
    EVP_MD_CTX_free(context_);
#endif
  }

  void Update(const char* bytes, std::size_t size) {
#if defined(_WIN32)
    if (size > std::numeric_limits<ULONG>::max() ||
        BCryptHashData(hash_,
                       reinterpret_cast<PUCHAR>(const_cast<char*>(bytes)),
                       static_cast<ULONG>(size), 0) != 0) {
      throw std::runtime_error("cannot update SHA-256");
    }
#elif defined(__APPLE__)
    if (size > std::numeric_limits<CC_LONG>::max() ||
        CC_SHA256_Update(&context_, bytes, static_cast<CC_LONG>(size)) != 1) {
      throw std::runtime_error("cannot update SHA-256");
    }
#else
    if (EVP_DigestUpdate(context_, bytes, size) != 1) {
      throw std::runtime_error("cannot update SHA-256");
    }
#endif
  }

  std::array<std::uint8_t, 32> Finish() {
    std::array<std::uint8_t, 32> digest{};
#if defined(_WIN32)
    if (BCryptFinishHash(hash_, digest.data(),
                         static_cast<ULONG>(digest.size()), 0) != 0) {
      throw std::runtime_error("cannot finish SHA-256");
    }
#elif defined(__APPLE__)
    if (CC_SHA256_Final(digest.data(), &context_) != 1) {
      throw std::runtime_error("cannot finish SHA-256");
    }
#else
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context_, digest.data(), &size) != 1 ||
        size != digest.size()) {
      throw std::runtime_error("cannot finish SHA-256");
    }
#endif
    return digest;
  }

 private:
#if defined(_WIN32)
  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
#elif defined(__APPLE__)
  CC_SHA256_CTX context_{};
#else
  EVP_MD_CTX* context_ = nullptr;
#endif
};

struct FileSignature {
  std::uint64_t size;
  std::int64_t modification_time;
  std::uint64_t device = 0;
  std::uint64_t inode = 0;
  std::int64_t change_time = 0;
};

std::string PathUtf8(const std::filesystem::path& path) {
  const std::u8string value = path.u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

FileSignature Signature(const std::filesystem::path& path) {
#if !defined(_WIN32)
  struct stat details{};
  if (stat(path.c_str(), &details) != 0)
    throw std::system_error(errno, std::system_category(),
                            "cannot stat model " + path.string());
  if (!S_ISREG(details.st_mode) || details.st_size < 0) {
    throw std::runtime_error("model is not a regular file: " + path.string());
  }
  FileSignature result{
      .size = static_cast<std::uint64_t>(details.st_size),
#if defined(__APPLE__)
      .modification_time =
          static_cast<std::int64_t>(details.st_mtimespec.tv_sec) *
              1000000000LL +
          details.st_mtimespec.tv_nsec,
#else
      .modification_time =
          static_cast<std::int64_t>(details.st_mtim.tv_sec) * 1000000000LL +
          details.st_mtim.tv_nsec,
#endif
      .device = static_cast<std::uint64_t>(details.st_dev),
      .inode = static_cast<std::uint64_t>(details.st_ino),
#if defined(__APPLE__)
      .change_time = static_cast<std::int64_t>(details.st_ctimespec.tv_sec) *
                         1000000000LL +
                     details.st_ctimespec.tv_nsec};
#else
      .change_time =
          static_cast<std::int64_t>(details.st_ctim.tv_sec) * 1000000000LL +
          details.st_ctim.tv_nsec};
#endif
  return result;
#else
  HANDLE handle =
      CreateFileW(path.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(),
                            "cannot inspect model " + path.string());
  BY_HANDLE_FILE_INFORMATION info{};
  FILE_BASIC_INFO basic{};
  const bool ok = GetFileInformationByHandle(handle, &info) &&
                  GetFileInformationByHandleEx(handle, FileBasicInfo, &basic,
                                               sizeof(basic));
  const int last_error = ok ? 0 : static_cast<int>(GetLastError());
  CloseHandle(handle);
  if (!ok)
    throw std::system_error(last_error, std::system_category(),
                            "cannot inspect model " + path.string());
  const std::uint64_t write =
      (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
      info.ftLastWriteTime.dwLowDateTime;
  const std::uint64_t changed =
      static_cast<std::uint64_t>(basic.ChangeTime.QuadPart);
  FileSignature result{
      .size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) |
              info.nFileSizeLow,
      .modification_time = static_cast<std::int64_t>(write),
      .device = info.dwVolumeSerialNumber,
      .inode = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) |
               info.nFileIndexLow,
      .change_time = static_cast<std::int64_t>(changed)};
  return result;
#endif
}

bool Same(const FileSignature& left, const FileSignature& right) {
  return left.size == right.size &&
         left.modification_time == right.modification_time &&
         left.device == right.device && left.inode == right.inode &&
         left.change_time == right.change_time;
}

std::string PathKey(const std::filesystem::path& path) {
  const auto utf8 = path.generic_u8string();
  return Sha256Hex(std::string_view(reinterpret_cast<const char*>(utf8.data()),
                                    utf8.size()));
}

std::filesystem::path MemoPath(const std::filesystem::path& model) {
  const auto base = EntropyCacheBaseDirectory();
  cache_io::EnsurePrivateDirectory(base);
  const auto directory = base / "model-digests";
  cache_io::EnsurePrivateDirectory(directory);
  return directory / (PathKey(model) + ".json");
}

std::optional<std::string> ReadMemo(const std::filesystem::path& path,
                                    const FileSignature& signature) {
  try {
    cache_io::CheckNotSymlink(path.parent_path());
    cache_io::CheckNotSymlink(path);
    std::error_code error;
    if (!std::filesystem::is_regular_file(
            std::filesystem::symlink_status(path, error)) ||
        error)
      return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    nlohmann::json value = nlohmann::json::parse(input);
    if (!value.is_object() || !value.contains("digest") ||
        !value["digest"].is_string())
      return std::nullopt;
    const FileSignature saved{
        .size = value.at("size").get<std::uint64_t>(),
        .modification_time = value.at("mtime").get<std::int64_t>(),
        .device = value.at("device").get<std::uint64_t>(),
        .inode = value.at("inode").get<std::uint64_t>(),
        .change_time = value.at("ctime").get<std::int64_t>()};
    const auto digest = value["digest"].get<std::string>();
    if (!Same(saved, signature) || digest.size() != 64 ||
        digest.find_first_not_of("0123456789abcdef") != std::string::npos)
      return std::nullopt;
    return digest;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::string HashFile(const std::filesystem::path& path) {
  const std::string display_path = PathUtf8(path);
  ReportPhase("hashing model " + display_path);
  const auto started = std::chrono::steady_clock::now();
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open model " + display_path);
  NativeSha256 hash;
  // Keep hashing bounded in memory, including on platforms with small stacks.
  std::array<char, std::size_t{64} * 1024> buffer{};
  std::uint64_t completed = 0;
  const auto total = std::filesystem::file_size(path);
  while (input.read(buffer.data(), buffer.size()) || input.gcount() != 0) {
    completed += static_cast<std::uint64_t>(input.gcount());
    ReportCounter(completed, total, "bytes");
    hash.Update(buffer.data(), static_cast<std::size_t>(input.gcount()));
  }
  if (!input.eof())
    throw std::runtime_error("cannot read model " + display_path);
  const auto digest = hash.Finish();
  static constexpr std::array kDigits = {'0', '1', '2', '3', '4', '5',
                                         '6', '7', '8', '9', 'a', 'b',
                                         'c', 'd', 'e', 'f'};
  std::string result;
  result.reserve(64);
  for (const auto byte : digest) {
    result.push_back(kDigits[byte >> 4]);
    result.push_back(kDigits[byte & 15]);
  }
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started)
          .count();
  ReportPhase("model hash complete bytes=" + std::to_string(completed) +
              " duration_ms=" + std::to_string(milliseconds));
  return result;
}

void WriteMemo(const std::filesystem::path& path,
               const FileSignature& signature, std::string_view digest) {
  nlohmann::json value{
      {"size", signature.size},         {"mtime", signature.modification_time},
      {"device", signature.device},     {"inode", signature.inode},
      {"ctime", signature.change_time}, {"digest", digest}};
  cache_io::CheckNotSymlink(path.parent_path());
  cache_io::AtomicWriteFile(path, value.dump());
}

struct HashedFile {
  std::filesystem::path path;
  FileSignature signature;
  std::string digest;
};

HashedFile InspectFile(const std::filesystem::path& path) {
  FileSignature stable = Signature(path);
  std::optional<std::filesystem::path> memo;
  try {
    memo = MemoPath(path);
    if (const auto cached = ReadMemo(*memo, stable);
        cached.has_value() && Same(stable, Signature(path))) {
      return {path, stable, *cached};
    }
  } catch (const std::exception&) {
    memo.reset();  // Digest storage is advisory.
  }
  FileSignature current = stable;
  for (int attempt = 0; attempt != 2; ++attempt) {
    auto digest = HashFile(path);
    const FileSignature after = Signature(path);
    if (Same(current, after)) {
      if (memo.has_value()) {
        try {
          WriteMemo(*memo, after, digest);
        } catch (const std::exception&) {
          // Validated model content is usable even if memo storage fails.
        }
      }
      return {path, after, std::move(digest)};
    }
    if (attempt == 1)
      throw std::runtime_error("model changed while hashing: " + path.string());
    current = after;
  }
  throw std::runtime_error("cannot hash model " + path.string());
}

std::vector<std::filesystem::path> ModelFiles(
    const std::filesystem::path& canonical) {
  const auto utf8_name = canonical.filename().u8string();
  const std::string name(reinterpret_cast<const char*>(utf8_name.data()),
                         utf8_name.size());
  static const std::regex split_pattern(
      R"((.*)-([0-9]{5})-of-([0-9]{5})\.gguf)");
  std::smatch match;
  if (!std::regex_match(name, match, split_pattern)) return {canonical};
  const auto index = std::stoul(match[2].str());
  const auto count = std::stoul(match[3].str());
  if (count <= 1 || index == 0 || index > count) return {canonical};

  std::vector<std::filesystem::path> files;
  files.reserve(count);
  for (std::size_t shard = 1; shard <= count; ++shard) {
    std::ostringstream filename;
    filename << match[1].str() << '-' << std::setfill('0') << std::setw(5)
             << shard << "-of-" << std::setw(5) << count << ".gguf";
    const std::string bytes = filename.str();
    const std::u8string value(bytes.begin(), bytes.end());
    std::error_code error;
    auto path = std::filesystem::canonical(
        canonical.parent_path() / std::filesystem::path(value), error);
    if (error) {
      throw std::runtime_error("cannot resolve model shard " + bytes + ": " +
                               error.message());
    }
    files.push_back(std::move(path));
  }
  return files;
}

std::string ModelDigest(const std::vector<HashedFile>& files) {
  if (files.size() == 1) return files.front().digest;
  Sha256 hash;
  static constexpr std::string_view domain = "llm-cc-split-model-v1";
  hash.Update({domain.data(), domain.size()});
  for (const auto& file : files) {
    static constexpr char separator = '\0';
    hash.Update({&separator, 1});
    hash.Update({file.digest.data(), file.digest.size()});
  }
  const auto digest = hash.Finish();
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 15]);
  }
  return result;
}

}  // namespace

ModelIdentity InspectModel(const std::filesystem::path& model,
                           std::string_view inference_abi,
                           std::string_view backend,
                           std::uint32_t context_limit,
                           std::uint32_t batch_size,
                           std::string_view reduction_policy,
                           std::string_view effective_reducer,
                           bool cache_enabled, std::string_view flash_attention,
                           std::string_view kv_cache_type, bool kv_offload) {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(model, error);
  if (error)
    throw std::runtime_error("cannot resolve model " + model.string() + ": " +
                             error.message());
  FileSignature stable = Signature(canonical);
  std::string digest;
  std::uint64_t total_size = stable.size;
  if (cache_enabled) {
    const auto paths = ModelFiles(canonical);
    for (int attempt = 0; attempt != 2; ++attempt) {
      std::vector<HashedFile> files;
      files.reserve(paths.size());
      for (const auto& path : paths) files.push_back(InspectFile(path));
      bool unchanged = true;
      for (const auto& file : files) {
        if (!Same(file.signature, Signature(file.path))) {
          unchanged = false;
          break;
        }
      }
      if (unchanged) {
        total_size = 0;
        stable.modification_time = std::numeric_limits<std::int64_t>::min();
        for (const auto& file : files) {
          if (file.signature.size >
              std::numeric_limits<std::uint64_t>::max() - total_size) {
            throw std::runtime_error("model size overflow");
          }
          total_size += file.signature.size;
          stable.modification_time = std::max(stable.modification_time,
                                              file.signature.modification_time);
        }
        digest = ModelDigest(files);
        break;
      }
      if (attempt == 1)
        throw std::runtime_error("model shards changed while hashing: " +
                                 canonical.string());
    }
  }
  return {.canonical_path = canonical,
          .size = total_size,
          .modification_time = stable.modification_time,
          .inference_abi = std::string(inference_abi),
          .backend = std::string(backend),
          .context_limit = context_limit,
          .batch_size = batch_size,
          .reduction_policy = std::string(reduction_policy),
          .effective_reducer = std::string(effective_reducer),
          .flash_attention = std::string(flash_attention),
          .kv_cache_type = std::string(kv_cache_type),
          .kv_offload = kv_offload,
          .content_digest = std::move(digest)};
}

}  // namespace llmcc
