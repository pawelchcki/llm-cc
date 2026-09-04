#include "src/cache.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <tuple>

namespace llmcc {
namespace {

constexpr std::string_view kManifestFile = "models.json";

std::optional<std::filesystem::path> EnvironmentPath(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::filesystem::path(value);
}

#if defined(_WIN32)
std::optional<std::filesystem::path> EnvironmentPath(const wchar_t* name) {
  const wchar_t* value = _wgetenv(name);
  if (value == nullptr || *value == L'\0') {
    return std::nullopt;
  }
  return std::filesystem::path(value);
}
#endif

std::optional<std::filesystem::path> NativeEnvironmentPath(
    const char* narrow_name, const wchar_t* wide_name) {
#if defined(_WIN32)
  static_cast<void>(narrow_name);
  return EnvironmentPath(wide_name);
#else
  static_cast<void>(wide_name);
  return EnvironmentPath(narrow_name);
#endif
}

std::filesystem::path ManifestPath(const std::filesystem::path& cache_dir) {
  return cache_dir / kManifestFile;
}

void WriteManifest(const std::filesystem::path& cache_dir,
                   const ModelManifest& manifest) {
  try {
    std::filesystem::create_directories(cache_dir);
    nlohmann::json contents = nlohmann::json::object();
    for (const auto& [file_name, timestamps] : manifest) {
      nlohmann::json value = nlohmann::json::object();
      if (timestamps.downloaded_at.has_value()) {
        value["downloaded_at"] = *timestamps.downloaded_at;
      }
      if (timestamps.last_used_at.has_value()) {
        value["last_used_at"] = *timestamps.last_used_at;
      }
      contents[file_name] = std::move(value);
    }
    std::ofstream output(ManifestPath(cache_dir), std::ios::binary);
    output << contents.dump(2);
  } catch (const std::exception& error) {
    // Cache metadata is advisory; model use must not fail because of it.
    static_cast<void>(error);
  }
}

std::uint64_t EpochSeconds() {
  const auto duration = std::chrono::system_clock::now().time_since_epoch();
  if (duration < std::chrono::system_clock::duration::zero()) {
    return 0;
  }
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(duration).count());
}

bool IsBareFileName(std::string_view file_name) {
  const bool bare = !file_name.empty() && file_name != "." &&
                    file_name != ".." &&
                    file_name.find('/') == std::string_view::npos &&
                    file_name.find('\\') == std::string_view::npos;
#if defined(_WIN32)
  return bare && file_name.find(':') == std::string_view::npos;
#else
  return bare;
#endif
}
void RemoveIfPresent(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("failed to remove " + path.string() + ": " +
                             error.message());
  }
}

std::tuple<std::int64_t, std::int64_t, std::int64_t> CivilDate(
    std::int64_t days_since_epoch) {
  const std::int64_t days = days_since_epoch + 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const std::int64_t day_of_era = days - (era * 146097);
  const std::int64_t year_of_era =
      (day_of_era - (day_of_era / 1460) + (day_of_era / 36524) -
       (day_of_era / 146096)) /
      365;
  std::int64_t year = year_of_era + (era * 400);
  const std::int64_t day_of_year =
      day_of_era -
      ((365 * year_of_era) + (year_of_era / 4) - (year_of_era / 100));
  const std::int64_t month_prime = ((5 * day_of_year) + 2) / 153;
  const std::int64_t day = day_of_year - (((153 * month_prime) + 2) / 5) + 1;
  const std::int64_t month = month_prime + (month_prime < 10 ? 3 : -9);
  year += month <= 2 ? 1 : 0;
  return {year, month, day};
}

}  // namespace

std::filesystem::path CacheDir() {
  if (const auto override_dir =
          NativeEnvironmentPath("LLM_CC_CACHE_DIR", L"LLM_CC_CACHE_DIR");
      override_dir.has_value() && !override_dir->empty()) {
    return *override_dir;
  }
  if (const auto xdg =
          NativeEnvironmentPath("XDG_CACHE_HOME", L"XDG_CACHE_HOME");
      xdg.has_value() && !xdg->empty()) {
    return *xdg / "llm-cc/models";
  }
  if (const auto home = NativeEnvironmentPath("HOME", L"HOME");
      home.has_value() && !home->empty()) {
    return *home / ".cache/llm-cc/models";
  }
#if defined(_WIN32)
  if (const auto local_app_data =
          NativeEnvironmentPath("LOCALAPPDATA", L"LOCALAPPDATA");
      local_app_data.has_value() && !local_app_data->empty()) {
    return *local_app_data / "llm-cc/models";
  }
  if (const auto user_profile =
          NativeEnvironmentPath("USERPROFILE", L"USERPROFILE");
      user_profile.has_value() && !user_profile->empty()) {
    return *user_profile / "AppData/Local/llm-cc/models";
  }
#endif
  throw std::runtime_error("cache root is unavailable; set LLM_CC_CACHE_DIR");
}

std::filesystem::path CacheDirFrom(
    const std::optional<std::filesystem::path>& override_dir,
    const std::optional<std::filesystem::path>& xdg_cache_home,
    const std::optional<std::filesystem::path>& home) {
  if (override_dir.has_value() && !override_dir->empty()) {
    return *override_dir;
  }
  if (xdg_cache_home.has_value() && !xdg_cache_home->empty()) {
    return *xdg_cache_home / "llm-cc/models";
  }
  if (home.has_value() && !home->empty()) {
    return *home / ".cache/llm-cc/models";
  }
  throw std::runtime_error(
      "cannot determine model cache directory; set LLM_CC_CACHE_DIR");
}

std::filesystem::path PartialPath(const std::filesystem::path& target) {
  auto partial = target;
  partial += std::filesystem::path(".partial");
  return partial;
}

ModelManifest ReadManifest(const std::filesystem::path& cache_dir) {
  try {
    std::ifstream input(ManifestPath(cache_dir), std::ios::binary);
    const nlohmann::json contents = nlohmann::json::parse(input);
    if (!contents.is_object()) {
      return {};
    }
    ModelManifest manifest;
    for (const auto& [file_name, value] : contents.items()) {
      if (!value.is_object()) {
        continue;
      }
      ModelTimestamps timestamps;
      if (value.contains("downloaded_at") &&
          value["downloaded_at"].is_number_unsigned()) {
        timestamps.downloaded_at = value["downloaded_at"].get<std::uint64_t>();
      }
      if (value.contains("last_used_at") &&
          value["last_used_at"].is_number_unsigned()) {
        timestamps.last_used_at = value["last_used_at"].get<std::uint64_t>();
      }
      manifest.emplace(file_name, timestamps);
    }
    return manifest;
  } catch (const std::exception&) {
    return {};
  }
}

void MarkModelDownloaded(const std::filesystem::path& model) {
  if (!model.has_parent_path() || model.filename().empty()) {
    return;
  }
  ModelManifest manifest = ReadManifest(model.parent_path());
  manifest[model.filename().string()].downloaded_at = EpochSeconds();
  WriteManifest(model.parent_path(), manifest);
}

void MarkCachedModelUsed(const std::filesystem::path& cache_dir,
                         const std::filesystem::path& model) {
  if (!model.has_parent_path() || model.filename().empty()) {
    return;
  }
  std::error_code error;
  bool cached = model.parent_path() == cache_dir;
  if (!cached) {
    const auto model_parent =
        std::filesystem::weakly_canonical(model.parent_path(), error);
    error.clear();
    const auto canonical_cache =
        std::filesystem::weakly_canonical(cache_dir, error);
    cached = !error && model_parent == canonical_cache;
  }
  if (!cached) {
    return;
  }
  ModelManifest manifest = ReadManifest(cache_dir);
  manifest[model.filename().string()].last_used_at = EpochSeconds();
  WriteManifest(cache_dir, manifest);
}

std::string FormatTimestamp(std::optional<std::uint64_t> timestamp) {
  if (!timestamp.has_value()) {
    return "never";
  }
  if (*timestamp >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::to_string(*timestamp);
  }
  const auto seconds_since_epoch = static_cast<std::int64_t>(*timestamp);
  const std::int64_t days = seconds_since_epoch / 86400;
  const std::int64_t seconds = seconds_since_epoch % 86400;
  const auto [year, month, day] = CivilDate(days);
  std::ostringstream output;
  output << std::setfill('0') << std::setw(4) << year << '-' << std::setw(2)
         << month << '-' << std::setw(2) << day << ' ' << std::setw(2)
         << seconds / 3600 << ':' << std::setw(2) << seconds % 3600 / 60 << ':'
         << std::setw(2) << seconds % 60 << " UTC";
  return output.str();
}

void ListModels(const std::filesystem::path& cache_dir, std::ostream& output) {
  const ModelManifest manifest = ReadManifest(cache_dir);
  std::error_code error;
  const bool exists = std::filesystem::exists(cache_dir, error);
  if (error) {
    throw std::runtime_error("failed to inspect model cache " +
                             cache_dir.string() + ": " + error.message());
  }
  if (!exists) {
    return;
  }
  std::map<std::string, std::uintmax_t> models;
  for (const auto& entry : std::filesystem::directory_iterator(cache_dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".gguf") {
      models.emplace(entry.path().filename().string(), entry.file_size());
    }
  }
  for (const auto& [file_name, size] : models) {
    const auto iterator = manifest.find(file_name);
    const ModelTimestamps timestamps =
        iterator == manifest.end() ? ModelTimestamps{} : iterator->second;
    output << file_name << '\t' << std::fixed << std::setprecision(2)
           << static_cast<double>(size) / (1024.0 * 1024.0 * 1024.0) << " GiB\t"
           << FormatTimestamp(timestamps.downloaded_at) << '\t'
           << FormatTimestamp(timestamps.last_used_at) << '\n';
  }
}

void RemoveModel(const std::filesystem::path& cache_dir,
                 std::string_view file_name) {
  if (!IsBareFileName(file_name)) {
    throw std::invalid_argument(
        "model name must be a bare file name without path separators");
  }
  const std::filesystem::path target = cache_dir / file_name;
  RemoveIfPresent(target);
  RemoveIfPresent(PartialPath(target));
  ModelManifest manifest = ReadManifest(cache_dir);
  manifest.erase(std::string(file_name));
  WriteManifest(cache_dir, manifest);
}

std::filesystem::path ResolveModel(
    std::optional<std::filesystem::path> model, const ModelSpec& spec,
    bool no_download, const std::filesystem::path& current_dir,
    const std::filesystem::path& cache_dir,
    const std::function<void(std::string_view, const std::filesystem::path&)>&
        downloader) {
  if (model.has_value()) {
    return *model;
  }
  std::filesystem::path legacy = current_dir / "models" / spec.file;
  if (std::filesystem::exists(legacy)) {
    return legacy;
  }
  const std::filesystem::path cached = cache_dir / spec.file;
  if (!std::filesystem::exists(cached)) {
    if (no_download) {
      throw std::runtime_error("model " + std::string(spec.name) +
                               " is not cached at " + cached.string() +
                               "; remove --no-download to fetch it "
                               "automatically");
    }
    downloader(spec.url, cached);
  }
  return cached;
}

std::filesystem::path ResolveModel(
    std::optional<std::filesystem::path> model, bool no_download,
    const std::filesystem::path& current_dir,
    const std::filesystem::path& cache_dir,
    const std::function<void(const std::filesystem::path&)>& downloader) {
  return ResolveModel(
      std::move(model), DefaultModel(), no_download, current_dir, cache_dir,
      [&](std::string_view, const std::filesystem::path& target) {
        downloader(target);
      });
}

}  // namespace llmcc
