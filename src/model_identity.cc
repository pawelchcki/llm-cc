#include "src/model_identity.h"

#include <array>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>

#include "src/cache_io.h"
#include "src/sha256.h"

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

struct FileSignature {
  std::uint64_t size;
  std::int64_t modification_time;
  std::uint64_t device = 0;
  std::uint64_t inode = 0;
  std::int64_t change_time = 0;
};

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
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open model " + path.string());
  Sha256 hash;
  // Keep hashing bounded in memory, including on platforms with small stacks.
  std::array<char, 64 * 1024> buffer{};
  while (input.read(buffer.data(), buffer.size()) || input.gcount() != 0) {
    hash.Update(std::span<const char>(
        buffer.data(), static_cast<std::size_t>(input.gcount())));
  }
  if (!input.eof())
    throw std::runtime_error("cannot read model " + path.string());
  const auto digest = hash.Finish();
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (const auto byte : digest) {
    result.push_back(kDigits[byte >> 4]);
    result.push_back(kDigits[byte & 15]);
  }
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

}  // namespace

ModelIdentity InspectModel(
    const std::filesystem::path& model, std::string_view inference_abi,
    std::string_view backend, std::uint32_t context_limit,
    std::uint32_t batch_size, std::string_view reduction_policy,
    std::string_view effective_reducer, bool cache_enabled) {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(model, error);
  if (error)
    throw std::runtime_error("cannot resolve model " + model.string() + ": " +
                             error.message());
  FileSignature stable = Signature(canonical);
  std::string digest;
  std::optional<std::filesystem::path> memo;
  if (cache_enabled) {
    try {
      memo = MemoPath(canonical);
      if (const auto cached = ReadMemo(*memo, stable);
          cached.has_value() && Same(stable, Signature(canonical))) {
        digest = *cached;
      }
    } catch (const std::exception&) {
      memo.reset();  // Digest storage is advisory.
    }
  }
  if (cache_enabled && digest.empty()) {
    FileSignature current = stable;
    for (int attempt = 0; attempt != 2; ++attempt) {
      digest = HashFile(canonical);
      const FileSignature after = Signature(canonical);
      if (Same(current, after)) {
        stable = after;
        if (memo.has_value()) {
          try {
            WriteMemo(*memo, after, digest);
          } catch (const std::exception&) {
            // Validated model content is usable even if memo storage fails.
          }
        }
        break;
      }
      if (attempt == 1)
        throw std::runtime_error("model changed while hashing: " +
                                 canonical.string());
      current = after;
    }
  }
  return {.canonical_path = canonical,
          .size = stable.size,
          .modification_time = stable.modification_time,
          .inference_abi = std::string(inference_abi),
          .backend = std::string(backend),
          .context_limit = context_limit,
          .batch_size = batch_size,
          .reduction_policy = std::string(reduction_policy),
          .effective_reducer = std::string(effective_reducer),
          .content_digest = std::move(digest)};
}

}  // namespace llmcc
