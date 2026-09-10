#include "src/backend.h"

#include <ggml-backend.h>
#include <llama.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "generated/backend_locations.h"
#include "generated/build_config.h"
#include "src/backend_fetch.h"
#include "src/payload.h"
#include "src/progress.h"
#include "src/rocm_topology.h"

#ifdef __linux__
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace llmcc {
namespace {

void RecordBackendLog(ggml_log_level level, const char* text, void* user_data) {
  if (text != nullptr) {
    static_cast<BackendLogCapture*>(user_data)->Record(
        level == GGML_LOG_LEVEL_ERROR, text);
  }
}

class MissingBackendPluginError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

constexpr std::string_view PluginName(BackendKind backend) {
  switch (backend) {
    case BackendKind::kCpu:
      return "libllm-cc-backend-cpu.so";
    case BackendKind::kCuda:
      return "libllm-cc-backend-cuda.so";
    case BackendKind::kRocm:
      return "libllm-cc-backend-rocm.so";
    case BackendKind::kAuto:
      break;
  }
  return {};
}

std::string BundleName(BackendKind backend) {
  return std::string(BackendName(backend)) + ".bundle";
}

std::filesystem::path ChecksumPath(const std::filesystem::path& bundle) {
  return std::filesystem::path(bundle.string() + ".sha256");
}

bool IsRegularFile(const std::filesystem::path& path) {
  std::error_code error;
  const bool regular = std::filesystem::is_regular_file(path, error);
  if (error == std::errc::no_such_file_or_directory ||
      error == std::errc::not_a_directory) {
    return false;
  }
  if (error) {
    throw std::runtime_error("could not inspect backend path " + path.string() +
                             ": " + error.message());
  }
  return regular;
}

void ValidateBackendDirectory(const std::filesystem::path& directory) {
  std::error_code error;
  if (std::filesystem::is_directory(directory, error)) {
    return;
  }
  std::string message =
      "backend directory is not a directory: " + directory.string();
  if (error) {
    message += ": " + error.message();
  }
  throw std::runtime_error(message);
}

std::optional<ResolvedBackendPlugin> ResolveConfiguredPlugin(
    BackendKind backend, const std::filesystem::path& directory,
    std::string_view version, std::string_view git_sha) {
  ValidateBackendDirectory(directory);
  std::vector<std::pair<std::filesystem::path, BackendPluginSource>> candidates;
  if (const auto artifact = BackendArtifactName(BackendName(backend));
      artifact.has_value()) {
    candidates.emplace_back(directory / (*artifact + ".bundle"),
                            BackendPluginSource::kBundle);
  }
  candidates.emplace_back(directory / BundleName(backend),
                          BackendPluginSource::kBundle);
  candidates.emplace_back(directory / PluginName(backend),
                          BackendPluginSource::kSharedLibrary);
  for (const auto& [path, source] : candidates) {
    if (IsRegularFile(path)) {
      if (source == BackendPluginSource::kBundle) {
        VerifyBackendBundle({.name = BackendName(backend),
                             .version = version,
                             .git_sha = git_sha},
                            path);
      }
      return ResolvedBackendPlugin{
          .source = source,
          .path = path,
          .payload_verified = source == BackendPluginSource::kBundle};
    }
  }
  return std::nullopt;
}

bool HasInstalledArtifact(const std::filesystem::path& installed_bundle,
                          const std::filesystem::path& installed_checksum,
                          const std::filesystem::path& installed_manifest) {
  for (const std::filesystem::path& path : {
           installed_bundle,
           installed_checksum,
           installed_manifest,
           std::filesystem::path(installed_bundle.string() + ".partial"),
           std::filesystem::path(installed_checksum.string() + ".partial"),
           std::filesystem::path(installed_manifest.string() + ".partial"),
       }) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    if (!error && status.type() != std::filesystem::file_type::not_found) {
      return true;
    }
    if (error && error != std::errc::no_such_file_or_directory &&
        error != std::errc::not_a_directory) {
      throw std::runtime_error("could not inspect installed backend " +
                               path.string() + ": " + error.message());
    }
  }
  return false;
}

std::optional<ResolvedBackendPlugin> ResolveInstalledBundle(
    BackendKind backend, std::string_view version, std::string_view git_sha,
    const std::function<std::optional<std::filesystem::path>()>&
        installed_root) {
  if (!installed_root) {
    return std::nullopt;
  }
  const auto root = installed_root();
  if (!root.has_value()) {
    return std::nullopt;
  }
  BackendFetchOptions installed_options{
      .name = BackendName(backend),
      .version = version,
      .git_sha = git_sha,
      .runtime_root = *root,
  };
  const std::filesystem::path installed_bundle =
      BackendBundlePath(installed_options);
  const std::filesystem::path installed_checksum =
      ChecksumPath(installed_bundle);
  const std::filesystem::path installed_manifest =
      installed_bundle.parent_path() /
      (std::string(BackendName(backend)) + ".manifest.json");

  try {
    if (HasInstalledArtifact(installed_bundle, installed_checksum,
                             installed_manifest)) {
      VerifyBackendBundle(installed_options);
      return ResolvedBackendPlugin{
          .source = BackendPluginSource::kInstalledBundle,
          .path = installed_bundle,
          .payload_verified = true};
    }
  } catch (const std::exception& error) {
    throw std::runtime_error(
        "invalid installed " + std::string(BackendName(backend)) +
        " backend bundle at " + installed_bundle.string() + ": " +
        error.what() + "; reinstall with 'bazel run --config=" +
        std::string(BackendName(backend)) + " //:install'");
  }
  return std::nullopt;
}

#ifdef LLM_CC_DYNAMIC_BACKENDS

std::optional<std::filesystem::path> ExecutablePath() {
#ifdef __linux__
  std::vector<char> buffer(1024);
  for (;;) {
    const ssize_t count =
        readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (count < 0) {
      return std::nullopt;
    }
    if (static_cast<std::size_t>(count) < buffer.size()) {
      const std::filesystem::path executable(
          std::string(buffer.data(), static_cast<std::size_t>(count)));
      std::error_code error;
      const std::filesystem::path canonical =
          std::filesystem::canonical(executable, error);
      return error ? executable : canonical;
    }
    buffer.resize(buffer.size() * 2);
  }
#else
  return std::nullopt;
#endif
}

std::optional<std::filesystem::path> InstalledBackendRoot() {
  const auto executable = ExecutablePath();
  if (!executable.has_value()) {
    return std::nullopt;
  }
  return executable->parent_path().parent_path() / "lib" / "llm-cc";
}

std::vector<std::filesystem::path> PluginCandidates(BackendKind backend) {
  const std::filesystem::path file(PluginName(backend));
  std::vector<std::filesystem::path> candidates;
  if (const auto executable = ExecutablePath(); executable.has_value()) {
    candidates.push_back(executable->parent_path() / "lib" / file);
    candidates.push_back(executable->string() + ".runfiles/" +
                         LLM_CC_BACKEND_RUNFILES_DIR + "/" + file.string());
    candidates.push_back(executable->parent_path() / LLM_CC_BACKEND_EXEC_DIR /
                         file);
  }
  if (const char* runfiles = std::getenv("RUNFILES_DIR");
      runfiles != nullptr && *runfiles != '\0') {
    const std::filesystem::path root(runfiles);
    candidates.push_back(root / LLM_CC_BACKEND_RUNFILES_DIR / file);
  }
  return candidates;
}

struct LoadedPlugin {
  BackendKind backend;
  ggml_backend_reg_t registry;
  int backing_fd = -1;
  void* driver_handle = nullptr;
  bool hardware_detected = false;
  bool missing_with_download_disabled = false;
  std::exception_ptr failure;
};

std::string MissingGpuBackendMessage(BackendKind backend) {
  const std::string name(BackendName(backend));
  return name +
         " GPU offload requires a local backend bundle; install source-built "
         "support with 'bazel run --config=" +
         name +
         " //:install', fetch one explicitly with 'llm-cc backends fetch " +
         name + " [--url URL]', or use CPU with '--backend cpu --gpu-layers 0'";
}

std::string GpuOffloadHelp() {
  return "install local support with 'bazel run --config=cuda //:install' or "
         "'bazel run --config=rocm //:install', fetch a bundle explicitly "
         "with 'llm-cc backends fetch cuda|rocm [--url URL]', or use CPU "
         "with '--backend cpu --gpu-layers 0'";
}

LoadedPlugin LoadPlugin(
    BackendKind backend, bool required,
    const std::optional<std::filesystem::path>& backend_directory,
    std::string_view version, bool no_download, bool fetch_backend) {
#ifdef __linux__
  void* driver_handle = nullptr;
#endif
  const auto close_driver = [&]() {
#ifdef __linux__
    if (driver_handle != nullptr) {
      dlclose(driver_handle);
      driver_handle = nullptr;
    }
#endif
  };
#ifdef __linux__
  if (backend == BackendKind::kCuda) {
    driver_handle = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (driver_handle == nullptr) {
      const char* error = dlerror();
      const std::string message =
          "could not load NVIDIA driver libcuda.so.1: " +
          std::string(error != nullptr ? error : "unknown loader error");
      if (required) {
        throw std::runtime_error(message + "; " + GpuOffloadHelp());
      }
      return {.backend = backend,
              .registry = nullptr,
              .failure = std::make_exception_ptr(std::runtime_error(message))};
    }
  }
#endif
#ifdef __linux__
  if (backend == BackendKind::kCuda && driver_handle != nullptr) {
    using Init = int (*)(unsigned int);
    using Count = int (*)(int*);
    auto init = reinterpret_cast<Init>(dlsym(driver_handle, "cuInit"));
    auto count =
        reinterpret_cast<Count>(dlsym(driver_handle, "cuDeviceGetCount"));
    int devices = 0;
    if (!init || !count || init(0) != 0 || count(&devices) != 0 ||
        devices == 0) {
      close_driver();
      const std::string message = "no CUDA device found by driver probe";
      if (required) throw std::runtime_error(message + "; " + GpuOffloadHelp());
      return {.backend = backend,
              .registry = nullptr,
              .failure = std::make_exception_ptr(std::runtime_error(message))};
    }
  }
#endif
  std::optional<RocmTopology> rocm_topology;
  if (backend == BackendKind::kRocm) {
    rocm_topology = ConfigureRocmVisibility();
    // Containers can expose topology without granting access to /dev/kfd.
    // Loading or downloading a plugin cannot make that device usable.
    if (rocm_topology.has_value() && rocm_topology->has_supported_device &&
        !RocmDeviceAccessible()) {
      const std::string message =
          "an AMD GPU is visible but /dev/kfd is not accessible; grant this "
          "process read-write access to /dev/kfd";
      if (required) throw std::runtime_error(message + "; " + GpuOffloadHelp());
      return {.backend = backend,
              .registry = nullptr,
              .hardware_detected = true,
              .failure = std::make_exception_ptr(std::runtime_error(message))};
    }
    // An inconclusive hardware probe must not initiate a network download.
    if (!rocm_topology.has_value()) fetch_backend = false;
    if (rocm_topology.has_value() && !rocm_topology->has_supported_device) {
      if (required && rocm_topology->has_unsupported_device) {
        throw std::runtime_error(RocmUnsupportedSystemMessage(*rocm_topology) +
                                 "; " + GpuOffloadHelp());
      }
      return {
          .backend = backend,
          .registry = nullptr,
          .hardware_detected = rocm_topology->has_unsupported_device,
          .failure = rocm_topology->has_unsupported_device
                         ? std::make_exception_ptr(std::runtime_error(
                               RocmUnsupportedSystemMessage(*rocm_topology)))
                         : nullptr};
    }
  }
  std::optional<PreparedPayload> prepared;
  ResolvedBackendPlugin resolved;
  try {
    std::function<std::optional<ResolvedBackendPlugin>()> fetch;
    if (fetch_backend && !no_download) {
      fetch = [backend, version] {
        const std::filesystem::path path = FetchBackendBundle(
            {.name = BackendName(backend), .version = version});
        return std::optional<ResolvedBackendPlugin>(
            {{.source = BackendPluginSource::kBundle,
              .path = path,
              .payload_verified = true}});
      };
    }
    resolved = ResolveBackendPlugin(
        backend, backend_directory, PluginCandidates(backend),
        [&]() {
          prepared = PrepareEmbeddedPayload(BackendName(backend));
          return prepared.has_value();
        },
        [] { return RuntimeRoot(); }, version,
        std::string_view{LLM_CC_GIT_SHA, sizeof(LLM_CC_GIT_SHA) - 1}, fetch,
        [] { return InstalledBackendRoot(); });
    if (resolved.source == BackendPluginSource::kBundle ||
        resolved.source == BackendPluginSource::kInstalledBundle) {
      prepared = PrepareEmbeddedPayloadFromFile(
          resolved.path, BackendName(backend), resolved.payload_verified);
      if (!prepared.has_value()) {
        throw std::runtime_error(
            "backend bundle does not begin with the expected " +
            std::string(BackendName(backend)) +
            " magic: " + resolved.path.string());
      }
    }
  } catch (const MissingBackendPluginError&) {
    const std::exception_ptr failure = std::current_exception();
    close_driver();
    if (required) {
      throw std::runtime_error(MissingGpuBackendMessage(backend));
    }
    const bool hardware_detected =
        backend != BackendKind::kRocm || rocm_topology.has_value();
    return {.backend = backend,
            .registry = nullptr,
            .hardware_detected = hardware_detected,
            .missing_with_download_disabled =
                hardware_detected && (no_download || !fetch_backend),
            .failure = (no_download || !fetch_backend) ? nullptr : failure};
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    close_driver();
    if (required) {
      throw;
    }
    return {.backend = backend,
            .registry = nullptr,
            .hardware_detected = true,
            .failure = failure};
  }
  const std::filesystem::path plugin_path =
      prepared.has_value() ? prepared->path : resolved.path;
  if (ggml_backend_reg_t registry = ggml_backend_load(plugin_path.c_str());
      registry != nullptr) {
    return {.backend = backend,
            .registry = registry,
            .backing_fd = prepared.has_value() ? prepared->backing_fd : -1,
            .driver_handle = driver_handle};
  }
#ifdef __linux__
  if (prepared.has_value() && prepared->backing_fd >= 0) {
    close(prepared->backing_fd);
  }
#endif
  close_driver();
  std::string failure_message;
  if (resolved.source == BackendPluginSource::kEmbedded) {
    failure_message = "could not load embedded " +
                      std::string(BackendName(backend)) + " backend";
  } else {
    failure_message = "could not load " + std::string(BackendName(backend)) +
                      " backend plugin: " + resolved.path.string();
  }
  if (required) {
    throw std::runtime_error(failure_message);
  }
  return {
      .backend = backend,
      .registry = nullptr,
      .hardware_detected = true,
      .failure = std::make_exception_ptr(std::runtime_error(failure_message))};
}

void UnloadPlugin(LoadedPlugin& plugin) {
  if (plugin.registry != nullptr) {
    ggml_backend_unload(plugin.registry);
    plugin.registry = nullptr;
  }
#ifdef __linux__
  if (plugin.backing_fd >= 0) {
    close(plugin.backing_fd);
    plugin.backing_fd = -1;
  }
  if (plugin.driver_handle != nullptr) {
    dlclose(plugin.driver_handle);
    plugin.driver_handle = nullptr;
  }
#endif
}

std::vector<BackendDevice> Inventory(std::span<const LoadedPlugin> plugins) {
  std::vector<BackendDevice> devices;
  for (const LoadedPlugin& plugin : plugins) {
    if (plugin.registry == nullptr || plugin.backend == BackendKind::kCpu) {
      continue;
    }
    for (std::size_t index = 0; index < ggml_backend_dev_count(); ++index) {
      ggml_backend_dev_t device = ggml_backend_dev_get(index);
      const enum ggml_backend_dev_type type = ggml_backend_dev_type(device);
      if (ggml_backend_dev_backend_reg(device) != plugin.registry ||
          (type != GGML_BACKEND_DEVICE_TYPE_GPU &&
           type != GGML_BACKEND_DEVICE_TYPE_IGPU)) {
        continue;
      }
      std::size_t free_memory = 0;
      std::size_t total_memory = 0;
      ggml_backend_dev_memory(device, &free_memory, &total_memory);
      devices.push_back(
          {.backend = plugin.backend,
           .free_memory = static_cast<std::uint64_t>(free_memory)});
    }
  }
  return devices;
}
#endif

}  // namespace

bool AutomaticBackendFetchEnabled() { return LLM_CC_AUTO_FETCH_BACKENDS != 0; }

bool AutomaticBackendFetchAllowed(bool requested) {
  return requested && AutomaticBackendFetchEnabled();
}

BackendLogCapture::BackendLogCapture(bool diagnostics)
    : diagnostics_enabled_(diagnostics) {
  llama_log_set(RecordBackendLog, this);
}

BackendLogCapture::~BackendLogCapture() { llama_log_set(nullptr, nullptr); }

std::string BackendLogCapture::Error() const {
  std::string result = errors_;
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
    result.pop_back();
  }
  return result;
}

std::string BackendLogCapture::Diagnostics() const {
  std::string result = diagnostics_;
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
    result.pop_back();
  }
  return result;
}

void BackendLogCapture::Record(bool error, std::string_view text) {
  constexpr std::size_t kDiagnosticLimit = std::size_t{16} * 1024;
  if (error) {
    errors_.append(text);
  }
  if (diagnostics_enabled_ && diagnostics_.size() < kDiagnosticLimit) {
    diagnostics_.append(text.substr(0, kDiagnosticLimit - diagnostics_.size()));
  }
}

void BackendLogCapture::Clear() {
  errors_.clear();
  diagnostics_.clear();
}

ResolvedBackendPlugin ResolveBackendPlugin(
    BackendKind backend,
    const std::optional<std::filesystem::path>& backend_directory,
    std::span<const std::filesystem::path> runfile_candidates,
    const std::function<bool()>& has_embedded_payload,
    const std::function<std::filesystem::path()>& runtime_root,
    std::string_view version, std::string_view git_sha,
    const std::function<std::optional<ResolvedBackendPlugin>()>& fetch_backend,
    const std::function<std::optional<std::filesystem::path>()>& installed_root,
    bool include_runtime_cache) {
  if (backend != BackendKind::kCuda && backend != BackendKind::kRocm) {
    throw std::invalid_argument("only CUDA and ROCm plugins can be resolved");
  }

  // Resolution order:
  //   1. configured directory (standalone bundles before a raw plugin),
  //   2. the executable's embedded payload,
  //   3. bundles installed beside the executable's prefix,
  //   4. beside-the-executable and Bazel runfiles candidates,
  //   5. the versioned runtime cache,
  //   6. the optional network-fetch seam below.
  if (backend_directory.has_value()) {
    if (auto configured = ResolveConfiguredPlugin(backend, *backend_directory,
                                                  version, git_sha)) {
      return *configured;
    }
  }

  // Step 2 deliberately stays between the configured directory and all
  // development-tree paths so the historical fat-ELF behavior is unchanged.
  if (has_embedded_payload && has_embedded_payload()) {
    return {.source = BackendPluginSource::kEmbedded, .path = {}};
  }

  if (auto installed =
          ResolveInstalledBundle(backend, version, git_sha, installed_root)) {
    return *installed;
  }

  for (const std::filesystem::path& candidate : runfile_candidates) {
    if (IsRegularFile(candidate)) {
      return {.source = BackendPluginSource::kSharedLibrary, .path = candidate};
    }
  }

  BackendFetchOptions cached_options{
      .name = BackendName(backend),
      .version = version,
      .git_sha = git_sha,
      .runtime_root = runtime_root(),
  };
  const std::filesystem::path cached_bundle = BackendBundlePath(cached_options);
  if (include_runtime_cache && IsRegularFile(cached_bundle)) {
    try {
      VerifyBackendBundle(cached_options);
      return {.source = BackendPluginSource::kBundle,
              .path = cached_bundle,
              .payload_verified = true};
    } catch (const std::exception&) {
      if (!fetch_backend) {
        throw;
      }
    }
  }

  // Step 6 is deliberately invoked only after every local source misses.
  if (fetch_backend) {
    if (auto fetched = fetch_backend(); fetched.has_value()) {
      return *fetched;
    }
  }

  std::string message = "missing " + std::string(BackendName(backend)) +
                        " backend plugin " + std::string(PluginName(backend)) +
                        " (set --backend-dir or LLM_CC_BACKEND_DIR";
  if (backend_directory.has_value()) {
    message += "; searched backend directory " + backend_directory->string();
  }
  message +=
      ", the embedded executable, beside the executable and in Bazel "
      "runfiles, and runtime cache " +
      cached_bundle.string() + ")";
  throw MissingBackendPluginError(message);
}

std::string DiscoverBackendSource(BackendKind backend) {
#ifdef LLM_CC_DYNAMIC_BACKENDS
  std::optional<std::filesystem::path> directory;
  if (const char* env = std::getenv("LLM_CC_BACKEND_DIR"); env && *env)
    directory = env;
  try {
    const auto resolved = ResolveBackendPlugin(
        backend, directory, PluginCandidates(backend),
        [backend] { return HasEmbeddedPayload(BackendName(backend)); },
        [] { return RuntimeRoot(); }, LLM_CC_VERSION, LLM_CC_GIT_SHA, {},
        [] { return InstalledBackendRoot(); }, false);
    if (resolved.source == BackendPluginSource::kEmbedded)
      return "embedded (device untested)";
    return resolved.path.string() + " (device untested)";
  } catch (const MissingBackendPluginError&) {
    return "none (device untested)";
  } catch (const std::exception& error) {
    return "invalid: " + std::string(error.what()) + " (device untested)";
  }
#else
  static_cast<void>(backend);
  return "none (device untested)";
#endif
}

BackendRuntime::BackendRuntime(
    BackendKind requested, std::int32_t gpu_layers, std::string_view version,
    const std::optional<std::filesystem::path>& backend_directory,
    bool no_download, bool fetch_backend) {
  ReportPhase("resolving backend and probing GPU hardware");
  if (gpu_layers < -1) {
    throw std::invalid_argument("--gpu-layers must be -1 or greater");
  }
#ifdef LLM_CC_DYNAMIC_BACKENDS
  fetch_backend = AutomaticBackendFetchAllowed(fetch_backend);
  std::array<LoadedPlugin, 2> gpu_plugins{};
  std::size_t gpu_count = 0;
  const bool load_gpu = requested == BackendKind::kCuda ||
                        requested == BackendKind::kRocm ||
                        (gpu_layers != 0 && requested == BackendKind::kAuto);
  if (load_gpu && backend_directory.has_value()) {
    ValidateBackendDirectory(*backend_directory);
  }
  try {
    if (requested == BackendKind::kCuda ||
        (gpu_layers != 0 && requested == BackendKind::kAuto)) {
      gpu_plugins[gpu_count++] =
          LoadPlugin(BackendKind::kCuda, requested == BackendKind::kCuda,
                     backend_directory, version, no_download, fetch_backend);
    }
    if (requested == BackendKind::kRocm ||
        (gpu_layers != 0 && requested == BackendKind::kAuto)) {
      gpu_plugins[gpu_count++] =
          LoadPlugin(BackendKind::kRocm, requested == BackendKind::kRocm,
                     backend_directory, version, no_download, fetch_backend);
    }
    const std::vector<BackendDevice> devices =
        Inventory(std::span<const LoadedPlugin>(gpu_plugins.data(), gpu_count));
    if (devices.empty()) {
      // A missing bundle is recorded only after the corresponding hardware
      // probe succeeds. Prefer that actionable result over failures from an
      // unavailable optional hardware family.
      const auto missing =
          std::find_if(gpu_plugins.begin(), gpu_plugins.begin() + gpu_count,
                       [](const LoadedPlugin& plugin) {
                         return plugin.missing_with_download_disabled;
                       });
      if (missing != gpu_plugins.begin() + gpu_count) {
        throw std::runtime_error(MissingGpuBackendMessage(missing->backend));
      }
      auto failed = std::find_if(
          gpu_plugins.begin(), gpu_plugins.begin() + gpu_count,
          [](const LoadedPlugin& plugin) {
            return plugin.hardware_detected && plugin.failure != nullptr;
          });
      if (failed == gpu_plugins.begin() + gpu_count) {
        failed =
            std::find_if(gpu_plugins.begin(), gpu_plugins.begin() + gpu_count,
                         [](const LoadedPlugin& plugin) {
                           return plugin.failure != nullptr;
                         });
      }
      if (failed != gpu_plugins.begin() + gpu_count) {
        try {
          std::rethrow_exception(failed->failure);
        } catch (const std::exception& error) {
          throw std::runtime_error(std::string(error.what()) + "; " +
                                   GpuOffloadHelp());
        }
      }
    }
    try {
      selected_ = SelectBackend(requested, gpu_layers, devices);
    } catch (const std::runtime_error& error) {
      throw std::runtime_error(std::string(error.what()) + "; " +
                               GpuOffloadHelp());
    }
  } catch (...) {
    for (std::size_t index = 0; index < gpu_count; ++index) {
      UnloadPlugin(gpu_plugins[index]);
    }
    throw;
  }
  for (std::size_t index = 0; index < gpu_count; ++index) {
    LoadedPlugin& plugin = gpu_plugins[index];
    if (plugin.registry == nullptr) {
      continue;
    }
    if (plugin.backend != selected_) {
      UnloadPlugin(plugin);
      continue;
    }
    plugin_registry_ = plugin.registry;
    plugin_backing_fd_ = plugin.backing_fd;
    driver_handle_ = plugin.driver_handle;
    plugin.registry = nullptr;
    plugin.backing_fd = -1;
    plugin.driver_handle = nullptr;
  }
#else
  static_cast<void>(version);
  static_cast<void>(backend_directory);
  static_cast<void>(no_download);
  static_cast<void>(fetch_backend);
#ifdef LLM_CC_BUILTIN_GPU
  if (requested == BackendKind::kCuda || requested == BackendKind::kRocm) {
    throw std::runtime_error(std::string(BackendName(requested)) +
                             " backend is not included in this build");
  }
  if (requested == BackendKind::kCpu && gpu_layers != 0) {
    throw std::invalid_argument(
        "--backend cpu cannot be used with nonzero --gpu-layers");
  }
  selected_ = requested;
#else
  selected_ = SelectBackend(requested, gpu_layers, {});
  if (selected_ != BackendKind::kCpu && requested != BackendKind::kAuto) {
    throw std::runtime_error(std::string(BackendName(selected_)) +
                             " backend is not included in this build");
  }
#endif
#endif
  llama_backend_init();
  std::string description;
  for (std::size_t index = 0; index < ggml_backend_dev_count(); ++index) {
    auto device = ggml_backend_dev_get(index);
    const auto type = ggml_backend_dev_type(device);
    if (gpu_layers != 0 && (type == GGML_BACKEND_DEVICE_TYPE_GPU ||
                            type == GGML_BACKEND_DEVICE_TYPE_IGPU)) {
      if (!description.empty()) description += ", ";
      description += ggml_backend_dev_description(device);
    }
  }
  if (CliSessionActive() && gpu_layers != 0 && description.empty()) {
    llama_backend_free();
#ifdef LLM_CC_DYNAMIC_BACKENDS
    LoadedPlugin plugin{
        .backend = selected_,
        .registry = static_cast<ggml_backend_reg_t>(plugin_registry_),
        .backing_fd = plugin_backing_fd_,
        .driver_handle = driver_handle_};
    UnloadPlugin(plugin);
#endif
    throw std::runtime_error(
        "no supported GPU device is available; use --force-cpu to opt into CPU "
        "execution");
  }
  ReportPhase("resolved backend=" +
              std::string(selected_ == BackendKind::kAuto
                              ? "metal"
                              : BackendName(selected_)) +
              " device=" + (description.empty() ? "CPU" : description) +
              " gpu_layers=" + std::to_string(gpu_layers));
}

BackendRuntime::~BackendRuntime() {
  llama_backend_free();
#ifdef LLM_CC_DYNAMIC_BACKENDS
  LoadedPlugin plugin{
      .backend = selected_,
      .registry = static_cast<ggml_backend_reg_t>(plugin_registry_),
      .backing_fd = plugin_backing_fd_,
      .driver_handle = driver_handle_,
  };
  UnloadPlugin(plugin);
#endif
}

}  // namespace llmcc
