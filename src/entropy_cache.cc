#include "src/entropy_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace llmcc {
namespace {

std::string PathUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::u8string value = path.u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
#else
  return path.generic_string();
#endif
}

std::optional<std::filesystem::path> EnvironmentPath(const char* narrow_name,
                                                     const wchar_t* wide_name) {
#if defined(_WIN32)
  static_cast<void>(narrow_name);
  const wchar_t* value = _wgetenv(wide_name);
  return value != nullptr && *value != L'\0'
             ? std::optional<std::filesystem::path>(value)
             : std::nullopt;
#else
  static_cast<void>(wide_name);
  const char* value = std::getenv(narrow_name);
  return value != nullptr && *value != '\0'
             ? std::optional<std::filesystem::path>(value)
             : std::nullopt;
#endif
}

using Clock = std::chrono::system_clock;
constexpr std::string_view kLegacyNamespace = ".llm-cc-cache/llm-cc";

struct CacheLocation {
  std::filesystem::path base;
  std::filesystem::path directory;
};

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

std::uint64_t EpochSeconds() {
  const auto duration = Clock::now().time_since_epoch();
  return duration < Clock::duration::zero()
             ? 0
             : static_cast<std::uint64_t>(
                   std::chrono::duration_cast<std::chrono::seconds>(duration)
                       .count());
}

std::filesystem::path EntropyCacheBaseDirectory() {
  if (const auto override = EnvironmentPath("LLM_CC_ENTROPY_CACHE_DIR",
                                            L"LLM_CC_ENTROPY_CACHE_DIR")) {
    return *override;
  }
  if (const auto xdg = EnvironmentPath("XDG_CACHE_HOME", L"XDG_CACHE_HOME")) {
    return *xdg / "llm-cc/entropy";
  }
  if (const auto home = EnvironmentPath("HOME", L"HOME")) {
    return *home / ".cache/llm-cc/entropy";
  }
#if defined(_WIN32)
  if (const auto local_app_data =
          EnvironmentPath("LOCALAPPDATA", L"LOCALAPPDATA")) {
    return *local_app_data / "llm-cc/entropy";
  }
  if (const auto user_profile =
          EnvironmentPath("USERPROFILE", L"USERPROFILE")) {
    return *user_profile / "AppData/Local/llm-cc/entropy";
  }
#endif
  throw std::runtime_error(
      "cannot determine entropy cache directory; set "
      "LLM_CC_ENTROPY_CACHE_DIR");
}

CacheLocation ActiveLocation(const std::filesystem::path& repository) {
  const auto base = EntropyCacheBaseDirectory();
  std::error_code error;
  auto canonical = std::filesystem::weakly_canonical(repository, error);
  if (error) {
    canonical = repository.lexically_normal();
  }
  return {.base = base,
          .directory = base / Sha256Hex(PathUtf8(canonical)) / "v1/entropy"};
}

CacheLocation LegacyLocation(const std::filesystem::path& repository) {
  return {.base = repository,
          .directory = repository / kLegacyNamespace / "v1/entropy"};
}

void CheckCachePath(const CacheLocation& location) {
  const auto relative = location.directory.lexically_relative(location.base);
  if (relative.empty() || relative.is_absolute() ||
      (!relative.empty() && *relative.begin() == "..")) {
    throw std::runtime_error("invalid entropy cache path " +
                             location.directory.string());
  }
  std::filesystem::path path = location.base;
  for (const auto& component : relative) {
    path /= component;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
      throw std::runtime_error("cannot inspect cache path " + path.string() +
                               ": " + error.message());
    }
    if (!error && std::filesystem::is_symlink(status)) {
      throw std::runtime_error("refusing to follow cache-directory symlink " +
                               path.string());
    }
#if defined(_WIN32)
    if (!error) {
      const DWORD attributes = GetFileAttributesW(path.c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES &&
          (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        throw std::runtime_error(
            "refusing to follow cache-directory reparse point " +
            PathUtf8(path));
      }
    }
#endif
  }
}

void EnsureCacheDirectory(const CacheLocation& location) {
  CheckCachePath(location);
  std::error_code error;
  std::filesystem::create_directories(location.directory, error);
  if (error) {
    throw std::runtime_error("cannot create entropy cache " +
                             location.directory.string() + ": " +
                             error.message());
  }
#if !defined(_WIN32)
  const auto bucket = location.directory.parent_path().parent_path();
  const auto restrict = [&](const std::filesystem::path& path) {
    std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    if (error) {
      throw std::runtime_error("cannot restrict entropy cache " +
                               path.string() + ": " + error.message());
    }
  };
  auto path = bucket;
  restrict(path);
  for (const auto& component : location.directory.lexically_relative(bucket)) {
    path /= component;
    restrict(path);
  }
#endif
  CheckCachePath(location);
}

void WritePrivateFile(const std::filesystem::path& path,
                      std::span<const std::uint8_t> bytes) {
#if defined(_WIN32)
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write entropy cache entry");
  }
#else
  int descriptor =
      open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
           S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "failed to create entropy cache entry");
  }
  try {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const std::size_t count = std::min<std::size_t>(
          bytes.size() - offset,
          static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
      const ssize_t written = write(descriptor, bytes.data() + offset, count);
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "failed to write entropy cache entry");
      }
      if (written == 0) {
        throw std::runtime_error("failed to write entropy cache entry");
      }
      offset += static_cast<std::size_t>(written);
    }
    if (close(descriptor) != 0) {
      descriptor = -1;
      throw std::system_error(errno, std::generic_category(),
                              "failed to close entropy cache entry");
    }
    descriptor = -1;
  } catch (...) {
    if (descriptor >= 0) {
      close(descriptor);
    }
    throw;
  }
#endif
}

std::filesystem::path EntryPath(const CacheLocation& location,
                                std::string_view key) {
  return location.directory / (std::string(key) + ".cbor");
}

void Touch(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::last_write_time(
      path, std::filesystem::file_time_type::clock::now(), error);
}

struct EntryInfo {
  std::filesystem::path path;
  std::uint64_t size;
  std::filesystem::file_time_type used;
};

std::vector<EntryInfo> Entries(const CacheLocation& location) {
  CheckCachePath(location);
  std::error_code error;
  if (!std::filesystem::exists(location.directory, error)) {
    if (error) {
      throw std::runtime_error("cannot inspect entropy cache: " +
                               error.message());
    }
    return {};
  }
  std::vector<EntryInfo> result;
  for (const auto& entry :
       std::filesystem::directory_iterator(location.directory)) {
    const auto status = entry.symlink_status(error);
    if (error || std::filesystem::is_symlink(status) ||
        !std::filesystem::is_regular_file(status) ||
        entry.path().extension() != ".cbor") {
      error.clear();
      continue;
    }
    const auto size = entry.file_size(error);
    if (error || size > std::numeric_limits<std::uint64_t>::max()) {
      error.clear();
      continue;
    }
    const auto used = entry.last_write_time(error);
    if (error) {
      error.clear();
      continue;
    }
    result.push_back({.path = entry.path(),
                      .size = static_cast<std::uint64_t>(size),
                      .used = used});
  }
  return result;
}

bool IsComplete(std::string_view source,
                const std::vector<EntropyRecord>& records) {
  try {
    static_cast<void>(AlignTokens(source, records));
    for (std::size_t position = 0; position < records.size(); ++position) {
      const auto entropy = records[position].entropy;
      if ((!entropy.has_value() && position != 0) ||
          (entropy.has_value() &&
           (!std::isfinite(*entropy) || *entropy < 0.0))) {
        return false;
      }
    }
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

std::vector<EntropyRecord> Decode(const nlohmann::json& value,
                                  std::string_view source) {
  if (!value.is_object() || value.value("version", 0) != 1 ||
      !value.contains("records") || !value["records"].is_array() ||
      value.value("source_size", std::uint64_t{0}) != source.size()) {
    throw std::invalid_argument("invalid entropy cache entry");
  }
  std::vector<EntropyRecord> records;
  for (const auto& record : value["records"]) {
    if (!record.is_array() || record.size() != 2 || !record[0].is_binary()) {
      throw std::invalid_argument("invalid entropy cache record");
    }
    const auto& binary = record[0].get_binary();
    std::optional<double> entropy;
    if (record[1].is_null()) {
      if (!records.empty()) {
        throw std::invalid_argument(
            "only the first cached token may have null entropy");
      }
    } else {
      if (!record[1].is_number()) {
        throw std::invalid_argument("invalid cached entropy");
      }
      entropy = record[1].get<double>();
      if (!std::isfinite(*entropy) || *entropy < 0.0) {
        throw std::invalid_argument(
            "cached entropy must be finite and non-negative");
      }
    }
    records.push_back({.position = records.size(),
                       .bytes = std::string(binary.begin(), binary.end()),
                       .entropy = entropy});
  }
  if (!IsComplete(source, records)) {
    throw std::invalid_argument("incomplete entropy cache entry");
  }
  return records;
}

std::optional<std::vector<EntropyRecord>> ReadEntry(
    const CacheLocation& location, std::string_view key,
    std::string_view source, bool touch) {
  CheckCachePath(location);
  const auto path = EntryPath(location, key);
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (!error && std::filesystem::is_symlink(status)) {
    throw std::runtime_error("refusing to follow entropy-cache entry symlink " +
                             path.string());
  }
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("cannot inspect entropy cache entry " +
                             path.string() + ": " + error.message());
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input),
                                        std::istreambuf_iterator<char>()};
  auto records = Decode(nlohmann::json::from_cbor(bytes), source);
  if (touch) {
    Touch(path);
  }
  return records;
}

void MaybePrune(const std::filesystem::path& repository) {
  const auto location = ActiveLocation(repository);
  const auto marker = location.directory.parent_path() / ".last-prune";
  std::error_code error;
  const auto modified = std::filesystem::last_write_time(marker, error);
  if (!error && std::filesystem::file_time_type::clock::now() - modified <
                    std::chrono::hours(24)) {
    return;
  }
  PruneRepositoryCache(repository, true);
  EnsureCacheDirectory(location);
  std::ofstream output(marker, std::ios::binary | std::ios::trunc);
  output << EpochSeconds();
}

}  // namespace

std::string Sha256Hex(std::string_view contents) {
  std::vector<std::uint8_t> message(contents.begin(), contents.end());
  const std::uint64_t bit_length =
      static_cast<std::uint64_t>(contents.size()) * 8;
  message.push_back(0x80);
  while (message.size() % 64 != 56) {
    message.push_back(0);
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(static_cast<std::uint8_t>(bit_length >> shift));
  }
  std::array<std::uint32_t, 8> hash = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                       0xa54ff53a, 0x510e527f, 0x9b05688c,
                                       0x1f83d9ab, 0x5be0cd19};
  for (std::size_t block = 0; block < message.size(); block += 64) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      const std::size_t offset = block + (index * 4);
      words[index] = (static_cast<std::uint32_t>(message[offset]) << 24) |
                     (static_cast<std::uint32_t>(message[offset + 1]) << 16) |
                     (static_cast<std::uint32_t>(message[offset + 2]) << 8) |
                     message[offset + 3];
    }
    for (std::size_t index = 16; index < 64; ++index) {
      const std::uint32_t s0 = RotateRight(words[index - 15], 7) ^
                               RotateRight(words[index - 15], 18) ^
                               (words[index - 15] >> 3);
      const std::uint32_t s1 = RotateRight(words[index - 2], 17) ^
                               RotateRight(words[index - 2], 19) ^
                               (words[index - 2] >> 10);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = hash;
    for (std::size_t index = 0; index < 64; ++index) {
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
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
  }
  static constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (std::uint32_t word : hash) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      result.push_back(digits[(word >> shift) & 0xf]);
    }
  }
  return result;
}

ModelIdentity InspectModel(const std::filesystem::path& model,
                           std::string_view inference_abi,
                           std::string_view backend,
                           std::uint32_t context_limit,
                           std::uint32_t batch_size,
                           std::string_view reduction_policy,
                           std::string_view effective_reducer) {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(model, error);
  if (error) {
    throw std::runtime_error("cannot resolve model " + model.string() + ": " +
                             error.message());
  }
  const auto size = std::filesystem::file_size(canonical, error);
  if (error || size > std::numeric_limits<std::uint64_t>::max()) {
    throw std::runtime_error("cannot inspect model " + canonical.string() +
                             (error ? ": " + error.message() : ""));
  }
  const auto modified = std::filesystem::last_write_time(canonical, error);
  if (error) {
    throw std::runtime_error("cannot inspect model modification time: " +
                             error.message());
  }
  return {
      .canonical_path = canonical,
      .size = static_cast<std::uint64_t>(size),
      .modification_time =
          static_cast<std::int64_t>(modified.time_since_epoch().count()),
      .inference_abi = std::string(inference_abi),
      .backend = std::string(backend),
      .context_limit = context_limit,
      .batch_size = batch_size,
      .reduction_policy = std::string(reduction_policy),
      .effective_reducer = std::string(effective_reducer),
  };
}

std::string EntropyCacheKey(std::string_view source,
                            const ModelIdentity& model) {
  std::string identity = "llm-cc-entropy-v1";
  identity += '\0';
  identity += Sha256Hex(source);
  identity += '\0';
  identity += PathUtf8(model.canonical_path);
  identity += '\0' + std::to_string(model.size) + '\0' +
              std::to_string(model.modification_time) + '\0' +
              model.inference_abi + '\0' + model.backend + '\0' +
              std::to_string(model.context_limit) + '\0' +
              std::to_string(model.batch_size) + '\0' + model.reduction_policy +
              '\0' + model.effective_reducer;
  return Sha256Hex(identity);
}

std::filesystem::path RepositoryCacheDirectory(
    const std::filesystem::path& repository) {
  return ActiveLocation(repository).directory;
}

std::filesystem::path LegacyRepositoryCacheDirectory(
    const std::filesystem::path& repository) {
  return LegacyLocation(repository).directory;
}

EntropyCacheLookup ReadEntropyCache(const std::filesystem::path& repository,
                                    std::string_view source,
                                    const ModelIdentity& model) {
  try {
    const std::string key = EntropyCacheKey(source, model);
    if (auto records =
            ReadEntry(ActiveLocation(repository), key, source, true)) {
      MaybePrune(repository);
      return {.hit = true, .records = std::move(*records)};
    }
    auto legacy = ReadEntry(LegacyLocation(repository), key, source, false);
    if (!legacy.has_value()) {
      return {};
    }
    WriteEntropyCache(repository, source, model, *legacy);
    return {.hit = true, .records = std::move(*legacy)};
  } catch (const std::exception&) {
    return {};
  }
}

void WriteEntropyCache(const std::filesystem::path& repository,
                       std::string_view source, const ModelIdentity& model,
                       std::span<const EntropyRecord> records) {
  if (!IsComplete(source,
                  std::vector<EntropyRecord>(records.begin(), records.end()))) {
    throw std::invalid_argument("refusing to cache incomplete entropy records");
  }
  const auto location = ActiveLocation(repository);
  EnsureCacheDirectory(location);
  nlohmann::json encoded = {{"version", 1},
                            {"source_size", source.size()},
                            {"provenance",
                             {{"inference_abi", model.inference_abi},
                              {"backend", model.backend},
                              {"context_limit", model.context_limit},
                              {"batch_size", model.batch_size},
                              {"reduction_policy", model.reduction_policy},
                              {"effective_reducer", model.effective_reducer}}},
                            {"records", nlohmann::json::array()}};
  for (const auto& record : records) {
    const std::vector<std::uint8_t> bytes(record.bytes.begin(),
                                          record.bytes.end());
    encoded["records"].push_back(nlohmann::json::array(
        {nlohmann::json::binary(bytes), record.entropy.has_value()
                                            ? nlohmann::json(*record.entropy)
                                            : nlohmann::json(nullptr)}));
  }
  const auto target = EntryPath(location, EntropyCacheKey(source, model));
  static std::atomic<std::uint64_t> temporary_sequence = 0;
  const auto suffix =
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) +
      "." + std::to_string(temporary_sequence.fetch_add(1)) + "." +
      std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
  auto temporary = target;
  temporary += std::filesystem::path(".tmp." + suffix);
  try {
    const auto bytes = nlohmann::json::to_cbor(encoded);
    WritePrivateFile(temporary, bytes);
#ifdef _WIN32
    if (!MoveFileExW(std::filesystem::path(temporary).c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      throw std::system_error(static_cast<int>(GetLastError()),
                              std::system_category(),
                              "failed to install entropy cache entry");
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
      throw std::runtime_error("failed to install entropy cache entry: " +
                               error.message());
    }
#endif
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
  MaybePrune(repository);
}

RepositoryCacheStatus GetRepositoryCacheStatus(
    const std::filesystem::path& repository) {
  const auto active = ActiveLocation(repository);
  const auto legacy = LegacyLocation(repository);
  RepositoryCacheStatus status{
      .repository = repository,
      .directory = active.directory,
      .legacy_directory = legacy.directory,
  };
  const auto inspect = [&](const CacheLocation& location, bool is_legacy) {
    for (const auto& entry : Entries(location)) {
      if (is_legacy) {
        ++status.legacy_entries;
        status.legacy_bytes += entry.size;
      } else {
        ++status.entries;
        status.bytes += entry.size;
      }
      try {
        std::ifstream input(entry.path, std::ios::binary);
        const std::vector<std::uint8_t> bytes{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        const auto value = nlohmann::json::from_cbor(bytes);
        if (!value.is_object() || !value.contains("provenance") ||
            !value["provenance"].is_object() ||
            !value["provenance"].contains("inference_abi") ||
            !value["provenance"]["inference_abi"].is_string()) {
          ++status.unknown_provenance_entries;
          continue;
        }
        ++status.entries_by_inference_abi[value["provenance"]["inference_abi"]
                                              .get<std::string>()];
      } catch (const std::exception&) {
        ++status.malformed_entries;
      }
    }
  };
  inspect(active, false);
  inspect(legacy, true);
  return status;
}

void PruneRepositoryCache(const std::filesystem::path& repository, bool force) {
  static_cast<void>(force);
  const auto location = ActiveLocation(repository);
  auto entries = Entries(location);
  const auto now = std::filesystem::file_time_type::clock::now();
  std::uint64_t total = 0;
  for (const auto& entry : entries) {
    total += entry.size;
    if (now - entry.used > std::chrono::seconds(kEntropyCacheMaxAgeSeconds)) {
      std::error_code error;
      std::filesystem::remove(entry.path, error);
      if (!error) {
        total -= entry.size;
      }
    }
  }
  entries = Entries(location);
  total = 0;
  for (const auto& entry : entries) {
    total += entry.size;
  }
  std::ranges::sort(entries, {}, &EntryInfo::used);
  for (const auto& entry : entries) {
    if (total <= kEntropyCacheLimit) {
      break;
    }
    std::error_code error;
    std::filesystem::remove(entry.path, error);
    if (!error) {
      total -= entry.size;
    }
  }
}

void ClearRepositoryCache(const std::filesystem::path& repository) {
  const auto location = ActiveLocation(repository);
  CheckCachePath(location);
  const auto directory = location.directory.parent_path().parent_path();
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  if (error) {
    throw std::runtime_error("cannot clear llm-cc cache namespace: " +
                             error.message());
  }
}

void ClearLegacyRepositoryCache(const std::filesystem::path& repository) {
  const auto location = LegacyLocation(repository);
  CheckCachePath(location);
  const auto directory = repository / kLegacyNamespace;
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  if (error) {
    throw std::runtime_error("cannot clear legacy llm-cc cache namespace: " +
                             error.message());
  }
}

}  // namespace llmcc
