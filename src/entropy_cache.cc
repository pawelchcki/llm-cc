#include "src/entropy_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <system_error>

#include "src/cache_io.h"
#include "src/sha256.h"

namespace llmcc {
namespace {
std::atomic<uint64_t> g_now{0}, g_limit{0};
std::atomic<bool> g_delete_failure{false};
struct CacheLocation {
  std::filesystem::path base, directory;
};
uint64_t Now() {
  if (auto n = g_now.load()) return n;
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
uint64_t Limit() {
  const auto limit = g_limit.load();
  return limit != 0 ? limit : kEntropyCacheLimit;
}
CacheLocation GlobalLocation() {
  auto b = EntropyCacheBaseDirectory();
  return {b, b / "v2" / "entropy"};
}
CacheLocation RepositoryLocation(const std::filesystem::path& r) {
  std::error_code e;
  auto p = std::filesystem::weakly_canonical(r, e);
  if (e) p = r.lexically_normal();
  auto b = EntropyCacheBaseDirectory();
  const auto utf8 = p.generic_u8string();
  const auto key = Sha256Hex(std::string_view(
      reinterpret_cast<const char*>(utf8.data()), utf8.size()));
  return {b, b / key / "v1" / "entropy"};
}
CacheLocation LegacyLocation(const std::filesystem::path& r) {
  return {r, r / ".llm-cc-cache/llm-cc/v1/entropy"};
}
void CheckCachePath(const CacheLocation& l) {
  auto rel = l.directory.lexically_relative(l.base);
  if (rel.empty() || rel.is_absolute() || *rel.begin() == "..")
    throw std::runtime_error("invalid entropy cache path");
  auto p = l.base;
  cache_io::CheckNotSymlink(p);
  for (auto c : rel) {
    p /= c;
    std::error_code e;
    auto s = std::filesystem::symlink_status(p, e);
    if (e && e != std::errc::no_such_file_or_directory)
      throw std::runtime_error("cannot inspect cache: " + e.message());
    if (!e && std::filesystem::is_symlink(s))
      throw std::runtime_error("refusing cache symlink " + p.string());
#if defined(_WIN32)
    if (!e) {
      const DWORD attributes = GetFileAttributesW(p.c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES &&
          (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        throw std::runtime_error("refusing cache reparse point " + p.string());
      }
    }
#endif
  }
}
void EnsureCacheDirectory(const CacheLocation& location) {
  CheckCachePath(location);
  cache_io::EnsurePrivateDirectory(location.base);
  auto path = location.base;
  for (const auto& component :
       location.directory.lexically_relative(location.base)) {
    path /= component;
    cache_io::EnsurePrivateDirectory(path);
  }
  CheckCachePath(location);
}
struct EntryInfo {
  std::filesystem::path p;
  uint64_t bytes;
  std::optional<uint64_t> used;
};
std::optional<uint64_t> LastUsed(const std::filesystem::path& p) {
  std::error_code e;
  auto t = std::filesystem::last_write_time(p, e);
  if (e) return std::nullopt;
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::file_clock::to_sys(t).time_since_epoch())
      .count();
}
std::vector<EntryInfo> Entries(const CacheLocation& l) {
  CheckCachePath(l);
  std::vector<EntryInfo> v;
  std::error_code e;
  if (!std::filesystem::exists(l.directory, e)) {
    if (e) throw std::system_error(e, "cannot inspect entropy directory");
    return v;
  }
  for (auto& i : std::filesystem::directory_iterator(l.directory)) {
    auto s = i.symlink_status(e);
    if (!e && !std::filesystem::is_symlink(s) &&
        std::filesystem::is_regular_file(s) &&
        i.path().extension() == ".cbor") {
      auto b = i.file_size(e);
      if (e) throw std::system_error(e, "cannot measure entropy entry");
      v.push_back({i.path(), b, LastUsed(i.path())});
    }
    if (e) throw std::system_error(e, "cannot inspect entropy entry");
  }
  return v;
}
bool IsComplete(std::string_view s, const std::vector<EntropyRecord>& r) {
  try {
    static_cast<void>(AlignTokens(s, r));
    for (size_t i = 0; i < r.size(); ++i)
      if ((!r[i].entropy && i) ||
          (r[i].entropy &&
           (!std::isfinite(*r[i].entropy) || *r[i].entropy < 0)))
        return false;
    return true;
  } catch (...) {
    return false;
  }
}
nlohmann::json Provenance(std::string_view s, const ModelIdentity& m) {
  return {{"source_sha256", Sha256Hex(s)},
          {"model_sha256", m.content_digest},
          {"inference_abi", m.inference_abi},
          {"backend", m.backend},
          {"context_limit", m.context_limit},
          {"batch_size", m.batch_size},
          {"reduction_policy", m.reduction_policy},
          {"effective_reducer", m.effective_reducer}};
}
std::filesystem::path EntryPath(const CacheLocation& l, std::string_view k) {
  return l.directory / (std::string(k) + ".cbor");
}
std::optional<std::vector<EntropyRecord>> ReadEntry(const CacheLocation& l,
                                                    std::string_view k,
                                                    std::string_view s,
                                                    const ModelIdentity& m) {
  CheckCachePath(l);
  auto p = EntryPath(l, k);
  cache_io::CheckNotSymlink(p);
  std::error_code e;
  auto st = std::filesystem::symlink_status(p, e);
  if (!e && std::filesystem::is_symlink(st))
    throw std::runtime_error("refusing entropy entry symlink");
  if (e || !std::filesystem::is_regular_file(st)) return {};
  std::ifstream in(p, std::ios::binary);
  std::vector<uint8_t> b{std::istreambuf_iterator<char>(in), {}};
  auto j = nlohmann::json::from_cbor(b);
  if (!j.is_object() || j.value("version", 0) != 2 ||
      j.value("source_size", uint64_t{}) != s.size() ||
      !j.contains("provenance") || j["provenance"] != Provenance(s, m) ||
      !j.contains("records") || !j["records"].is_array())
    throw std::invalid_argument("invalid entropy entry");
  std::vector<EntropyRecord> r;
  for (auto& x : j["records"]) {
    if (!x.is_array() || x.size() != 2 || !x[0].is_binary())
      throw std::invalid_argument("invalid entropy record");
    std::optional<double> q;
    if (!x[1].is_null()) {
      if (!x[1].is_number()) throw std::invalid_argument("invalid entropy");
      q = x[1].get<double>();
    }
    auto& z = x[0].get_binary();
    r.push_back({r.size(), std::string(z.begin(), z.end()), q});
  }
  if (!IsComplete(s, r))
    throw std::invalid_argument("incomplete entropy entry");
  return r;
}
struct Accounting {
  uint64_t bytes = 0, entries = 0, next_expiry = 0;
  bool dirty = true;
};
std::filesystem::path AccountingPath(const CacheLocation& l) {
  return l.directory.parent_path() / "accounting.json";
}
Accounting RebuildAccounting(const CacheLocation& l) {
  Accounting a{.dirty = false};
  for (auto& e : Entries(l)) {
    a.bytes += e.bytes;
    ++a.entries;
    auto x = e.used.value_or(0) + kEntropyCacheMaxAgeSeconds;
    if (!a.next_expiry || x < a.next_expiry) a.next_expiry = x;
  }
  return a;
}
Accounting LoadAccounting(const CacheLocation& l) {
  try {
    cache_io::CheckNotSymlink(AccountingPath(l));
    std::ifstream in(AccountingPath(l));
    nlohmann::json j;
    in >> j;
    if (!j.is_object() || !j.contains("bytes") || !j.contains("entries") ||
        !j.contains("next_expiry") || !j["bytes"].is_number_unsigned() ||
        !j["entries"].is_number_unsigned() ||
        !j["next_expiry"].is_number_unsigned() ||
        (j.contains("dirty") && !j["dirty"].is_boolean())) {
      return RebuildAccounting(l);
    }
    Accounting a{j.at("bytes"), j.at("entries"),
                 j.value("next_expiry", uint64_t{}), j.value("dirty", true)};
    if ((a.entries == 0 && (a.bytes != 0 || a.next_expiry != 0)) ||
        (a.entries != 0 && (a.bytes == 0 || a.next_expiry == 0))) {
      return RebuildAccounting(l);
    }
    return a.dirty ? RebuildAccounting(l) : a;
  } catch (...) {
    return RebuildAccounting(l);
  }
}
void SaveAccounting(const CacheLocation& l, const Accounting& a) {
  auto p = AccountingPath(l);
  auto str = nlohmann::json{
      {"bytes", a.bytes},
      {"entries", a.entries},
      {"next_expiry", a.next_expiry},
      {"dirty",
       a.dirty}}.dump();
  cache_io::AtomicWriteFile(p, str);
}
bool RemoveEntry(const std::filesystem::path& p) {
  if (g_delete_failure.load()) return false;
  std::error_code e;
  return std::filesystem::remove(p, e) && !e;
}
void TouchEntry(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::last_write_time(
      path,
      std::chrono::file_clock::from_sys(
          std::chrono::system_clock::from_time_t(Now())),
      error);
}

bool Expired(std::optional<uint64_t> used) {
  return used.has_value() && Now() >= *used &&
         Now() - *used >= kEntropyCacheMaxAgeSeconds;
}

bool SweepDue(const CacheLocation& location) {
  const auto last = LastUsed(location.directory.parent_path() / ".last-sweep");
  return !last.has_value() || (Now() >= *last && Now() - *last >= 86400);
}

void CleanTemporaries(const CacheLocation& location) {
  // All entropy and metadata temporary writes hold the permanent lock, so any
  // matching temporary found while holding it belongs to an abandoned write.
  for (const auto& directory :
       {location.directory, location.directory.parent_path()}) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      const auto name = entry.path().filename().string();
      const bool temporary = directory == location.directory
                                 ? name.find(".cbor.tmp.") != std::string::npos
                                 : name.starts_with("accounting.json.tmp.") ||
                                       name.starts_with(".last-sweep.tmp.");
      if (temporary && !entry.is_directory()) RemoveEntry(entry.path());
    }
  }
}

void Sweep(const CacheLocation& location, Accounting& account) {
  for (const auto& entry : Entries(location)) {
    if (Expired(entry.used)) RemoveEntry(entry.p);
  }
  CleanTemporaries(location);
  account = RebuildAccounting(location);
  const auto marker = location.directory.parent_path() / ".last-sweep";
  cache_io::AtomicWriteFile(marker, std::to_string(Now()));
  TouchEntry(marker);
}

void MarkDirty(const CacheLocation& location, const Accounting& account) {
  auto dirty = account;
  dirty.dirty = true;
  SaveAccounting(location, dirty);
}

void MaybeMaintain(const CacheLocation& location) {
  try {
    if (!SweepDue(location)) return;
    EnsureCacheDirectory(location);
    cache_io::FileLock lock(location.directory.parent_path() / ".lock");
    if (!SweepDue(location)) return;
    auto account = LoadAccounting(location);
    MarkDirty(location, account);
    Sweep(location, account);
    SaveAccounting(location, account);
  } catch (const std::exception&) {
    // Housekeeping is advisory, including for a validated cache hit.
  }
}
}  // namespace

std::filesystem::path EntropyCacheBaseDirectory() {
#if defined(_WIN32)
  const auto environment_path = [](const wchar_t* name) {
    const wchar_t* value = _wgetenv(name);
    return value != nullptr && *value != L'\0'
               ? std::optional<std::filesystem::path>(value)
               : std::nullopt;
  };
  if (const auto value = environment_path(L"LLM_CC_ENTROPY_CACHE_DIR"))
    return *value;
  if (const auto value = environment_path(L"XDG_CACHE_HOME"))
    return *value / "llm-cc/entropy";
  if (const auto value = environment_path(L"HOME"))
    return *value / ".cache/llm-cc/entropy";
  if (const auto value = environment_path(L"LOCALAPPDATA"))
    return *value / "llm-cc/entropy";
  if (const auto value = environment_path(L"USERPROFILE"))
    return *value / "AppData/Local/llm-cc/entropy";
#else
  if (auto x = std::getenv("LLM_CC_ENTROPY_CACHE_DIR"); x && *x) return x;
  if (auto x = std::getenv("XDG_CACHE_HOME"); x && *x)
    return std::filesystem::path(x) / "llm-cc/entropy";
  if (auto x = std::getenv("HOME"); x && *x)
    return std::filesystem::path(x) / ".cache/llm-cc/entropy";
#endif
  throw std::runtime_error(
      "cannot determine entropy cache directory; set LLM_CC_ENTROPY_CACHE_DIR");
}
std::filesystem::path GlobalEntropyCacheDirectory() {
  return GlobalLocation().directory;
}
std::string EntropyCacheKey(std::string_view s, const ModelIdentity& m) {
  std::string x = "llm-cc-entropy-v2";
  x += '\0';
  x += Sha256Hex(s);
  x += '\0';
  x += m.content_digest;
  x += '\0';
  x += m.inference_abi;
  x += '\0';
  x += m.backend;
  x += '\0';
  x += std::to_string(m.context_limit);
  x += '\0';
  x += std::to_string(m.batch_size);
  x += '\0';
  x += m.reduction_policy;
  x += '\0';
  x += m.effective_reducer;
  return Sha256Hex(x);
}
void CheckEntropyCacheAvailability() {
  auto l = GlobalLocation();
  CheckCachePath(l);
  std::error_code e;
  auto s = std::filesystem::symlink_status(l.directory, e);
  if (!e && !std::filesystem::is_directory(s))
    throw std::runtime_error("entropy cache path is not a directory");
}
EntropyCacheLookup ReadEntropyCache(std::string_view source,
                                    const ModelIdentity& model) {
  try {
    const auto location = GlobalLocation();
    const auto key = EntropyCacheKey(source, model);
    auto records = ReadEntry(location, key, source, model);
    if (!records.has_value()) {
      MaybeMaintain(location);
      return {};
    }
    const auto path = EntryPath(location, key);
    // Unknown timestamps cannot invalidate already validated entropy. Known
    // expired entries are misses and are never made fresh by a lookup.
    if (Expired(LastUsed(path))) {
      MaybeMaintain(location);
      return {};
    }
    try {
      cache_io::FileLock lock(location.directory.parent_path() / ".lock");
      CheckCachePath(location);
      cache_io::CheckNotSymlink(path);
      if (!Expired(LastUsed(path))) TouchEntry(path);
    } catch (const std::exception&) {
      // Refresh failure cannot lose records already validated above.
    }
    MaybeMaintain(location);
    return {.hit = true, .records = std::move(*records)};
  } catch (const std::exception&) {
    return {};
  }
}

void WriteEntropyCache(std::string_view source, const ModelIdentity& model,
                       std::span<const EntropyRecord> records) {
  const std::vector<EntropyRecord> copy(records.begin(), records.end());
  if (!IsComplete(source, copy)) {
    throw std::invalid_argument("refusing incomplete entropy records");
  }
  nlohmann::json encoded = {{"version", 2},
                            {"source_size", source.size()},
                            {"provenance", Provenance(source, model)},
                            {"records", nlohmann::json::array()}};
  for (const auto& record : records) {
    encoded["records"].push_back(
        nlohmann::json::array({nlohmann::json::binary(std::vector<uint8_t>(
                                   record.bytes.begin(), record.bytes.end())),
                               record.entropy ? nlohmann::json(*record.entropy)
                                              : nlohmann::json(nullptr)}));
  }
  const auto bytes = nlohmann::json::to_cbor(encoded);
  if (bytes.size() > Limit()) return;
  const auto location = GlobalLocation();
  const auto target = EntryPath(location, EntropyCacheKey(source, model));
  EnsureCacheDirectory(location);
  cache_io::FileLock lock(location.directory.parent_path() / ".lock");
  CheckCachePath(location);
  cache_io::CheckNotSymlink(target);
  std::error_code error;
  const auto status = std::filesystem::symlink_status(target, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::system_error(error, "cannot inspect entropy target");
  }
  if (!error && std::filesystem::exists(status) &&
      !std::filesystem::is_regular_file(status)) {
    throw std::runtime_error("entropy target is not a regular file");
  }
  auto account = LoadAccounting(location);
  MarkDirty(location, account);
  // next_expiry is conservative after touches: it can cause an early scan but
  // cannot hide an expired entry. Inserts normally only read the small record.
  if (SweepDue(location) ||
      (account.next_expiry != 0 && account.next_expiry <= Now())) {
    Sweep(location, account);
  }
  const auto previous_size = std::filesystem::file_size(target, error);
  if (!error) {
    if (account.entries == 0 || previous_size > account.bytes) {
      account = RebuildAccounting(location);
    }
    account.bytes -= previous_size;
    --account.entries;
  } else if (error != std::errc::no_such_file_or_directory) {
    throw std::system_error(error, "cannot measure entropy target");
  }
  if (account.bytes > Limit() - bytes.size()) {
    auto entries = Entries(location);
    std::ranges::sort(entries, {}, &EntryInfo::used);
    for (const auto& entry : entries) {
      if (account.bytes <= Limit() - bytes.size()) break;
      if (entry.p != target && RemoveEntry(entry.p)) {
        account.bytes -= entry.bytes;
        --account.entries;
      }
    }
  }
  if (account.bytes > Limit() - bytes.size()) {
    // A failed eviction may leave entries behind. RebuildAccounting also
    // includes any original target that was subtracted for replacement but
    // remains on disk.
    SaveAccounting(location, RebuildAccounting(location));
    return;
  }
  cache_io::AtomicWriteFile(
      target, std::string_view(reinterpret_cast<const char*>(bytes.data()),
                               bytes.size()));
  TouchEntry(target);
  ++account.entries;
  account.bytes += bytes.size();
  const auto expires =
      LastUsed(target).value_or(0) + kEntropyCacheMaxAgeSeconds;
  if (account.next_expiry == 0 || expires < account.next_expiry) {
    account.next_expiry = expires;
  }
  account.dirty = false;
  SaveAccounting(location, account);
}
EntropyCacheStatus GetEntropyCacheStatus(bool inspect_provenance) {
  auto l = GlobalLocation();
  EnsureCacheDirectory(l);
  cache_io::FileLock lock(l.directory.parent_path() / ".lock");
  EntropyCacheStatus s{.directory = l.directory, .limit = Limit()};
  if (!inspect_provenance) {
    const auto accounting = LoadAccounting(l);
    s.entries = accounting.entries;
    s.bytes = accounting.bytes;
    return s;
  }
  for (auto& e : Entries(l)) {
    ++s.entries;
    s.bytes += e.bytes;
    try {
      std::ifstream in(e.p, std::ios::binary);
      std::vector<uint8_t> b{std::istreambuf_iterator<char>(in), {}};
      auto j = nlohmann::json::from_cbor(b);
      ++s.entries_by_inference_abi
            [j.at("provenance").at("inference_abi").get<std::string>()];
    } catch (...) {
      ++s.malformed_entries;
    }
  }
  return s;
}
void PruneEntropyCache(bool force) {
  auto l = GlobalLocation();
  EnsureCacheDirectory(l);
  cache_io::FileLock z(l.directory.parent_path() / ".lock");
  if (!force && !SweepDue(l)) return;
  auto a = LoadAccounting(l);
  SaveAccounting(l, {a.bytes, a.entries, a.next_expiry, true});
  Sweep(l, a);
  auto es = Entries(l);
  std::ranges::sort(es, {}, &EntryInfo::used);
  for (auto& e : es) {
    if (a.bytes <= Limit()) break;
    if (RemoveEntry(e.p)) {
      a.bytes -= std::min(a.bytes, e.bytes);
      --a.entries;
    }
  }
  SaveAccounting(l, RebuildAccounting(l));
}
void ClearEntropyCache() {
  auto l = GlobalLocation();
  EnsureCacheDirectory(l);
  cache_io::FileLock z(l.directory.parent_path() / ".lock");
  const auto a = LoadAccounting(l);
  SaveAccounting(l, {a.bytes, a.entries, a.next_expiry, true});
  for (auto& e : Entries(l))
    if (!RemoveEntry(e.p))
      throw std::runtime_error("cannot clear entropy cache");
  CleanTemporaries(l);
  SaveAccounting(l, Accounting{.dirty = false});
}
std::filesystem::path RepositoryCacheDirectory(const std::filesystem::path& r) {
  return RepositoryLocation(r).directory;
}
std::filesystem::path LegacyRepositoryCacheDirectory(
    const std::filesystem::path& r) {
  return LegacyLocation(r).directory;
}
RepositoryCacheStatus GetRepositoryCacheStatus(const std::filesystem::path& r) {
  auto a = RepositoryLocation(r), b = LegacyLocation(r);
  RepositoryCacheStatus s{.repository = r,
                          .directory = a.directory,
                          .legacy_directory = b.directory};
  const auto inspect = [&](const EntryInfo& entry) {
    try {
      std::ifstream input(entry.p, std::ios::binary);
      const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),
                                       {}};
      const auto value = nlohmann::json::from_cbor(bytes);
      if (!value.contains("provenance") || !value["provenance"].is_object() ||
          !value["provenance"].contains("inference_abi") ||
          !value["provenance"]["inference_abi"].is_string()) {
        ++s.unknown_provenance_entries;
      } else {
        ++s.entries_by_inference_abi[value["provenance"]["inference_abi"]
                                         .get<std::string>()];
      }
    } catch (const std::exception&) {
      ++s.malformed_entries;
    }
  };
  for (auto& e : Entries(a)) {
    ++s.entries;
    s.bytes += e.bytes;
    inspect(e);
  }
  for (auto& e : Entries(b)) {
    ++s.legacy_entries;
    s.legacy_bytes += e.bytes;
    inspect(e);
  }
  return s;
}
void ClearRepositoryCache(const std::filesystem::path& r) {
  auto l = RepositoryLocation(r);
  CheckCachePath(l);
  std::error_code e;
  std::filesystem::remove_all(l.directory.parent_path().parent_path(), e);
  if (e) throw std::runtime_error("cannot clear old cache");
}
void ClearLegacyRepositoryCache(const std::filesystem::path& r) {
  auto l = LegacyLocation(r);
  CheckCachePath(l);
  std::error_code e;
  std::filesystem::remove_all(r / ".llm-cc-cache/llm-cc", e);
  if (e) throw std::runtime_error("cannot clear legacy cache");
}
void SetEntropyCacheTestNow(uint64_t x) { g_now = x; }
void SetEntropyCacheTestLimit(uint64_t x) { g_limit = x; }
void SetEntropyCacheTestDeleteFailure(bool x) { g_delete_failure = x; }
}  // namespace llmcc
