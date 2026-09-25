#if defined(_WIN32)
#include <io.h>
#define STDERR_FILENO 2
#define isatty _isatty
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include "src/analysis_session.h"
#include "src/analysis_totals.h"
#include "src/analyze.h"
#include "src/backend_fetch.h"
#include "src/build_info.h"
#include "src/cache.h"
#include "src/download.h"
#include "src/entropy_cache.h"
#include "src/input_limits.h"
#include "src/jsonl.h"
#include "src/lang.h"
#include "src/models.h"
#include "src/progress.h"
#include "src/project.h"
#include "src/rules.h"
#include "src/rules_cmd.h"
#include "src/score_cmd.h"
#include "src/scoring_settings.h"

namespace {

std::string PathUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::u8string value = path.u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
#else
  return path.string();
#endif
}

std::filesystem::path ChecksumPath(const std::filesystem::path& bundle) {
  auto checksum = bundle;
  checksum += std::filesystem::path(".sha256");
  return checksum;
}

std::filesystem::path PartialPath(const std::filesystem::path& path) {
  auto partial = path;
  partial += std::filesystem::path(".partial");
  return partial;
}
struct AnalyzeArguments {
  std::vector<std::filesystem::path> sources;
  std::optional<llmcc::Language> language;
  std::string language_name = "auto";
  std::optional<std::filesystem::path> model;
  std::optional<std::string> model_name;
  bool no_download = false;
  bool no_ignore = false;
  bool no_cache = false;
  bool assume_yes = false;
  bool backend_diagnostics = false;
  llmcc::ScoringSettings scoring;
  std::size_t hotspots = 10;
  std::string score_mode = "raw";
  std::string format = "jsonl";
  std::string progress = "auto";
};

constexpr std::string_view kUsageBeforeContext =
    "Usage:\n"
    "  llm-cc PATH... [--lang "
    "auto|rust|c|cpp|java|python|go|javascript|csharp] [OPTIONS]\n"
    "  llm-cc score (--model GGUF | --model-name NAME) "
    "[--prompt TEXT | --file PATH] [OPTIONS]\n"
    "  llm-cc models list [--available]|remove FILE|path\n"
    "  llm-cc backends list|fetch|path|remove\n"
    "  llm-cc backends fetch cuda|rocm [--url URL] [--assume-yes|-y]\n"
    "      [--no-download] [--progress auto|always|never]\n"
    "  llm-cc cache status|prune [PATH] [--format text|json]\n"
    "  llm-cc cache clear [PATH] [--legacy|--all] [--format text|json]\n"
    "  llm-cc rules show [PATH]|check FILE|explain PATH...\n\n"
    "Analysis options:\n"
    "  --lang NAME          infer per file with auto, or force every input to\n"
    "                       rust, c, cpp, java, python, go, javascript, or\n"
    "                       csharp (default: auto)\n"
    "  --include-headers     accepted for compatibility; headers are always\n"
    "                       discovered\n"
    "  --no-ignore           include ignored and rule-excluded source files\n"
    "  --no-cache            disable shared entropy caching\n"
    "  --no-download         do not fetch the model or backend bundle\n"
    "  --model GGUF          llama.cpp-compatible model\n"
    "  --model-name NAME     registered model (default: "
    "deepseek-coder-v2-lite-base-q6_k)\n"
    "  --score raw|lmcc|density|mean  headline score mode (default: raw, the\n"
    "                       paper's LM-CC; lmcc divides it by scored tokens)\n"
    "  --hierarchy structural|reference  hierarchy contract (default: "
    "structural)\n"
    "  --tau N               absolute entropy threshold in nats (default: the\n"
    "                       registered model's calibrated value, else 0.67)\n"
    "  --gpu-layers N        layers to offload (default: -1, all; 0 opts into "
    "CPU)\n"
    "  --force-cpu           explicitly use CPU with zero offload\n"
    "  -y, --assume-yes      accept missing model/backend downloads\n"
    "  --backend NAME        auto, cpu, cuda, or rocm (default: auto)\n"
    "  --batch-size N        decode rows per batch (default: 64)\n"
    "  --entropy-reduction M auto, host, or device (default: auto)\n"
    "  --flash-attn M       auto, on, or off (default: on)\n"
    "  --kv-cache-type T    f16, q8_0, or q4_0 for K and V (default: q8_0)\n"
    "  --kv-offload M       on or off (default: on)\n"
    "  --backend-diagnostics  show backend warnings, allocations, and bounded "
    "operation placement\n"
    "  --backend-dir DIR     GPU backend bundle/shared-library directory\n"
    "  --context N           tokens per inference window (default: ";

constexpr std::string_view kUsageAfterContext =
    ")\n"
    "  --tau-percentile N    use the Nth percentile instead of --tau\n"
    "  --hotspots N          hotspot lines per file (default: 10, 0 disables)\n"
    "  --format jsonl|text   output format (default: jsonl; json is an alias)\n"
    "  --progress M          auto, always, or never on stderr (default: auto)\n"
    "  --alpha N             branching weight (default: 0.8)\n"
    "  -V, --version         show the program version\n"
    "  -h, --help            show this help\n";

[[noreturn]] void Usage(std::string_view error = {}) {
  if (!error.empty()) {
    std::cerr << "error: " << error << "\n\n";
  }
  std::cerr << kUsageBeforeContext << llmcc::kDefaultContextSize
            << kUsageAfterContext;
  std::exit(error.empty() ? 0 : 2);
}

template <typename Number>
Number ParseNumber(std::string_view option, std::string_view value) {
  try {
    return llmcc::ParseOptionNumber<Number>(option, value);
  } catch (const llmcc::UsageError& error) {
    Usage(error.what());
  }
}

std::string ValidModelNames() {
  std::string result = "valid model names: ";
  bool first = true;
  for (const llmcc::ModelSpec& model : llmcc::Models()) {
    if (!first) {
      result += ", ";
    }
    result += model.name;
    first = false;
  }
  return result;
}

bool SetOutputOption(AnalyzeArguments& arguments, std::string_view option,
                     std::string_view value) {
  if (option == "--hotspots") {
    arguments.hotspots = ParseNumber<std::size_t>(option, value);
    return true;
  }
  if (option == "--score") {
    arguments.score_mode = value;
    if (arguments.score_mode != "raw" && arguments.score_mode != "lmcc" &&
        arguments.score_mode != "density" && arguments.score_mode != "mean") {
      Usage("--score expects raw, lmcc, density, or mean");
    }
    return true;
  }
  if (option == "--format") {
    arguments.format = value == "json" ? "jsonl" : std::string(value);
    if (arguments.format != "jsonl" && arguments.format != "text") {
      Usage("--format expects jsonl, json, or text");
    }
    return true;
  }
  if (option == "--progress") {
    arguments.progress = value;
    if (arguments.progress != "auto" && arguments.progress != "always" &&
        arguments.progress != "never") {
      Usage("--progress expects auto, always, or never");
    }
    return true;
  }
  return false;
}

void SetAnalyzeOption(AnalyzeArguments& arguments, std::string_view option,
                      std::string_view value) {
  try {
    if (llmcc::ParseScoringOption(arguments.scoring, option, value)) {
      return;
    }
  } catch (const llmcc::UsageError& error) {
    Usage(error.what());
  }
  if (SetOutputOption(arguments, option, value)) {
    return;
  }
  if (option == "--lang") {
    arguments.language_name = value;
    if (value == "auto") {
      arguments.language.reset();
    } else {
      arguments.language = llmcc::ParseLanguage(value);
      arguments.language_name = llmcc::LanguageName(*arguments.language);
    }
    return;
  }
  if (option == "--model") {
    arguments.model = std::filesystem::u8path(value);
    return;
  }
  if (option == "--model-name") {
    arguments.model_name = value;
    return;
  }
  Usage("unknown option: " + std::string(option));
}

AnalyzeArguments ParseAnalyzeArguments(int argc, char** argv) {
  AnalyzeArguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (option == "-h" || option == "--help") {
      Usage();
    }
    if (option == "-V" || option == "--version") {
      std::cout << "llm-cc " << llmcc::build_info::Version() << '\n';
      std::exit(0);
    }
    if (llmcc::ParseScoringFlag(arguments.scoring, option)) {
      continue;
    }
    if (option == "--assume-yes" || option == "-y") {
      arguments.assume_yes = true;
      continue;
    }
    if (option == "--no-download") {
      arguments.no_download = true;
      continue;
    }
    if (option == "--include-headers") {
      continue;
    }
    if (option == "--no-ignore") {
      arguments.no_ignore = true;
      continue;
    }
    if (option == "--no-cache") {
      arguments.no_cache = true;
      continue;
    }
    if (option == "--backend-diagnostics") {
      arguments.backend_diagnostics = true;
      continue;
    }
    if (!option.starts_with('-')) {
      arguments.sources.emplace_back(std::filesystem::u8path(option));
      continue;
    }
    if (index + 1 >= argc) {
      Usage(std::string(option) + " requires a value");
    }
    SetAnalyzeOption(arguments, option, argv[++index]);
  }
  if (arguments.model.has_value() && arguments.model_name.has_value()) {
    Usage("--model-name and --model are mutually exclusive");
  }
  if (arguments.model_name.has_value() &&
      llmcc::FindModel(*arguments.model_name) == nullptr) {
    Usage("unknown model name '" + *arguments.model_name + "'; " +
          ValidModelNames());
  }
  if (arguments.sources.empty()) {
    Usage("at least one source path is required unless a subcommand is used");
  }
  try {
    llmcc::ValidateScoringSettings(arguments.scoring);
  } catch (const llmcc::UsageError& error) {
    Usage(error.what());
  }
  return arguments;
}

std::string ReadFile(const std::filesystem::path& path) {
  return llmcc::ReadBoundedFile(path);
}

void Emit(const nlohmann::json& event) {
  std::cout << event.dump() << '\n' << std::flush;
  if (!std::cout) {
    throw std::runtime_error("failed to write output");
  }
}

using llmcc::TerminalSafe;

using llmcc::ProgressReporter;

int RunModels(int argc, char** argv) {
  if (argc < 3) {
    Usage("models requires list, remove, or path");
  }
  const std::filesystem::path cache_dir = llmcc::CacheDir();
  const std::string_view action = argv[2];
  if (action == "list" && argc == 3) {
    llmcc::ListModels(cache_dir, std::cout);
    return 0;
  }
  if (action == "list" && argc == 4 &&
      std::string_view(argv[3]) == "--available") {
    for (const llmcc::ModelSpec& model : llmcc::Models()) {
      const bool cached =
          std::filesystem::is_regular_file(cache_dir / model.file);
      std::cout << model.name << '\t'
                << llmcc::FormatApproxSize(model.approx_bytes) << '\t'
                << (cached ? "cached" : "not cached") << '\n';
    }
    return 0;
  }
  if (action == "path" && argc == 3) {
    std::cout << PathUtf8(cache_dir) << '\n';
    return 0;
  }
  if (action == "remove" && argc == 4) {
    llmcc::RemoveModel(cache_dir, argv[3]);
    return 0;
  }
  Usage("invalid models command");
}

std::string_view BackendBundleName(std::string_view name) {
  if (name != "cuda" && name != "rocm") {
    Usage("backend name must be cuda or rocm");
  }
  return name;
}

llmcc::BackendFetchOptions BackendOptions(
    std::string_view name,
    const std::optional<std::string>& explicit_url = std::nullopt) {
  llmcc::BackendFetchOptions options{.name = BackendBundleName(name)};
  options.explicit_url = explicit_url;
  return options;
}

void RemoveBackendFile(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("could not remove backend cache file " +
                             PathUtf8(path) + ": " + error.message());
  }
}

bool BackendBundleCached(const std::filesystem::path& path) {
  std::error_code error;
  const bool cached = std::filesystem::is_regular_file(path, error);
  if (error && error != std::errc::no_such_file_or_directory &&
      error != std::errc::not_a_directory) {
    throw std::runtime_error("could not inspect backend bundle " +
                             PathUtf8(path) + ": " + error.message());
  }
  return cached;
}

void ListBackend(std::string_view name) {
  const std::filesystem::path path =
      llmcc::BackendBundlePath(BackendOptions(name));
  const bool cached = BackendBundleCached(path);
  if (!cached) {
    std::cout << name << "\tbundle-cache=not cached\tlocal-source="
              << llmcc::DiscoverBackendSource(llmcc::ParseBackend(name))
              << '\n';
    return;
  }
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    throw std::runtime_error("could not measure backend bundle " +
                             PathUtf8(path) + ": " + error.message());
  }
  std::string status = "verified";
  try {
    llmcc::VerifyBackendBundle(BackendOptions(name));
  } catch (const std::exception&) {
    status = "invalid";
  }
  std::cout << name << "\tbundle-cache=" << status << "\t" << PathUtf8(path)
            << '\t' << size << " bytes\tlocal-source="
            << llmcc::DiscoverBackendSource(llmcc::ParseBackend(name)) << '\n';
}

void FetchBackend(int argc, char** argv) {
  const std::string_view name = BackendBundleName(argv[3]);
  std::optional<std::string> explicit_url;
  bool no_download = false;
  bool assume_yes = false;
  std::string progress_mode = "auto";
  for (int index = 4; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (option == "--no-download") {
      no_download = true;
    } else if (option == "--assume-yes" || option == "-y") {
      assume_yes = true;
    } else if (option == "--progress") {
      if (++index >= argc) Usage("--progress requires a value");
      progress_mode = argv[index];
    } else if (option == "--url") {
      if (explicit_url.has_value()) {
        Usage("--url may only be specified once");
      }
      if (++index >= argc) {
        Usage("--url requires a value");
      }
      explicit_url = argv[index];
    } else {
      Usage("invalid backends fetch option: " + std::string(option));
    }
  }
  ProgressReporter progress(progress_mode);
  llmcc::CliSession session(progress, assume_yes, no_download);
  progress.Phase("resolving backend bundle");
  std::cout << PathUtf8(llmcc::FetchBackendBundle(
                   BackendOptions(name, explicit_url)))
            << '\n';
}

void RemoveBackend(std::string_view name) {
  const std::filesystem::path bundle =
      llmcc::BackendBundlePath(BackendOptions(name));
  const bool cached = BackendBundleCached(bundle);
  const std::array files = {
      bundle,
      ChecksumPath(bundle),
      bundle.parent_path() / (std::string(name) + ".manifest.json"),
  };
  for (const std::filesystem::path& path : files) {
    RemoveBackendFile(path);
    RemoveBackendFile(PartialPath(path));
  }
  if (cached) {
    std::cout << name << "\tremoved\t" << PathUtf8(bundle) << '\n';
  } else {
    std::cout << name << "\tnot cached\n";
  }
}

int RunBackends(int argc, char** argv) {
  if (argc < 3) {
    Usage("backends requires list, fetch, path, or remove");
  }
  const std::string_view action = argv[2];
  if (action == "list" && argc == 3) {
    for (const std::string_view name : {"cuda", "rocm"}) {
      ListBackend(name);
    }
    return 0;
  }
  if (action == "fetch" && argc >= 4) {
    FetchBackend(argc, argv);
    return 0;
  }
  if (action == "path" && (argc == 3 || argc == 4)) {
    const llmcc::BackendFetchOptions options =
        BackendOptions(argc == 4 ? argv[3] : "cuda");
    const std::filesystem::path bundle = llmcc::BackendBundlePath(options);
    std::cout << PathUtf8(argc == 4 ? bundle : bundle.parent_path()) << '\n';
    return 0;
  }
  if (action == "remove" && argc == 4) {
    RemoveBackend(BackendBundleName(argv[3]));
    return 0;
  }
  Usage("invalid backends command");
}

struct CacheArguments {
  std::string_view action;
  std::optional<std::filesystem::path> path;
  std::string_view format = "text";
  bool legacy = false;
  bool all = false;
};

CacheArguments ParseCacheArguments(int argc, char** argv) {
  if (argc < 3) {
    Usage("cache requires status, prune, or clear");
  }
  CacheArguments arguments{.action = argv[2]};
  for (int index = 3; index < argc; ++index) {
    const std::string_view value = argv[index];
    if (value == "--format") {
      if (++index >= argc) {
        Usage("--format requires text or json");
      }
      arguments.format = argv[index];
      if (arguments.format != "text" && arguments.format != "json") {
        Usage("--format expects text or json");
      }
    } else if (value == "--legacy") {
      arguments.legacy = true;
    } else if (value == "--all") {
      arguments.all = true;
    } else if (!value.starts_with('-') && !arguments.path.has_value()) {
      arguments.path = std::filesystem::u8path(value);
    } else {
      Usage("invalid cache option: " + std::string(value));
    }
  }
  if (arguments.action != "status" && arguments.action != "prune" &&
      arguments.action != "clear") {
    Usage("cache requires status, prune, or clear");
  }
  if ((arguments.legacy || arguments.all) && arguments.action != "clear") {
    Usage("--legacy and --all are only valid with cache clear");
  }
  if (arguments.all && (arguments.legacy || arguments.path.has_value())) {
    Usage("cache clear --all cannot be combined with PATH or --legacy");
  }
  return arguments;
}

void PrintCacheJson(
    const llmcc::EntropyCacheStatus& status,
    const std::optional<llmcc::RepositoryCacheStatus>& repository_status,
    std::optional<std::string_view> cleared) {
  nlohmann::json output{
      {"scope", "user"},
      {"directory", PathUtf8(status.directory)},
      {"storage_version", status.storage_version},
      {"inference_abi", llmcc::InferenceAbi()},
      // External verifiers (tools/comparison) pin an installation to the
      // commit this executable was built from; the backend manifest only
      // describes the backend bundle.
      {"source_commit", std::string(llmcc::build_info::GitSha())},
      {"analysis_version", llmcc::kAnalysisVersion},
      {"backend_configuration",
       std::string(llmcc::build_info::BackendConfiguration())},
      {"entries", status.entries},
      {"bytes", status.bytes},
      {"limit_bytes", status.limit},
      {"retention_seconds", status.retention_seconds},
      {"entries_by_inference_abi", status.entries_by_inference_abi},
      {"malformed_entries", status.malformed_entries}};
  if (repository_status.has_value()) {
    output["repository_v1"] = {
        {"repository", PathUtf8(repository_status->repository)},
        {"directory", PathUtf8(repository_status->directory)},
        {"entries", repository_status->entries},
        {"bytes", repository_status->bytes},
        {"legacy_directory", PathUtf8(repository_status->legacy_directory)},
        {"legacy_entries", repository_status->legacy_entries},
        {"legacy_bytes", repository_status->legacy_bytes},
        {"unknown_provenance_entries",
         repository_status->unknown_provenance_entries},
        {"malformed_entries", repository_status->malformed_entries}};
  }
  if (cleared.has_value()) {
    output["cleared"] = *cleared;
  }
  std::cout << output.dump() << '\n';
}

void PrintCacheText(
    const llmcc::EntropyCacheStatus& status,
    const std::optional<llmcc::RepositoryCacheStatus>& repository_status,
    std::optional<std::string_view> cleared) {
  std::cout << "scope: user\n"
            << "directory: " << PathUtf8(status.directory) << '\n'
            << "storage version: " << status.storage_version << '\n'
            << "inference ABI: " << llmcc::InferenceAbi() << '\n'
            << "source commit: " << llmcc::build_info::GitSha() << '\n'
            << "analysis version: " << llmcc::kAnalysisVersion << '\n'
            << "backend configuration: "
            << llmcc::build_info::BackendConfiguration() << '\n'
            << "entries: " << status.entries << '\n'
            << "bytes: " << status.bytes << '\n'
            << "limit bytes: " << status.limit << '\n'
            << "retention seconds: " << status.retention_seconds << '\n'
            << "malformed entries: " << status.malformed_entries << '\n';
  if (repository_status.has_value()) {
    std::cout << "repository v1: " << PathUtf8(repository_status->repository)
              << '\n'
              << "repository v1 directory: "
              << PathUtf8(repository_status->directory) << '\n'
              << "repository v1 entries: " << repository_status->entries << '\n'
              << "repository v1 bytes: " << repository_status->bytes << '\n'
              << "legacy directory: "
              << PathUtf8(repository_status->legacy_directory) << '\n'
              << "legacy entries: " << repository_status->legacy_entries
              << '\n';
  }
  if (cleared.has_value()) {
    std::cout << "cleared: " << *cleared << '\n';
  }
}

std::string_view ClearCache(
    const CacheArguments& arguments,
    std::optional<llmcc::RepositoryCacheStatus>& repository_status) {
  if (arguments.all) {
    llmcc::ClearEntropyCache();
    return "shared v2 entropy cache";
  }
  const std::filesystem::path scoped_path =
      arguments.path.value_or(std::filesystem::current_path());
  const auto repository = llmcc::FindGitRepository(scoped_path);
  if (!repository.has_value()) {
    throw std::invalid_argument(PathUtf8(scoped_path) +
                                " is not inside a Git worktree");
  }
  llmcc::ClearRepositoryCache(*repository);
  if (arguments.legacy) {
    llmcc::ClearLegacyRepositoryCache(*repository);
  }
  repository_status = llmcc::GetRepositoryCacheStatus(*repository);
  return arguments.legacy ? "repository v1 and legacy entropy caches"
                          : "repository v1 entropy cache";
}

int RunCache(int argc, char** argv) {
  const CacheArguments arguments = ParseCacheArguments(argc, argv);
  std::optional<std::string_view> cleared;
  std::optional<llmcc::RepositoryCacheStatus> repository_status;
  if (arguments.path.has_value()) {
    const auto repository = llmcc::FindGitRepository(*arguments.path);
    if (!repository.has_value()) {
      throw std::invalid_argument(PathUtf8(*arguments.path) +
                                  " is not inside a Git worktree");
    }
    repository_status = llmcc::GetRepositoryCacheStatus(*repository);
  }
  if (arguments.action == "prune") {
    llmcc::PruneEntropyCache();
  } else if (arguments.action == "clear") {
    cleared = ClearCache(arguments, repository_status);
  }
  const auto status = llmcc::GetEntropyCacheStatus();
  if (arguments.format == "json") {
    PrintCacheJson(status, repository_status, cleared);
  } else {
    PrintCacheText(status, repository_status, cleared);
  }
  return 0;
}

llmcc::ModelRequest ModelRequestOf(const AnalyzeArguments& arguments) {
  return {.model = arguments.model, .model_name = arguments.model_name};
}

nlohmann::json RulesJson(const std::vector<llmcc::RulesSource>& sources) {
  nlohmann::json result = nlohmann::json::array();
  for (const llmcc::RulesSource& source : sources) {
    result.push_back(
        {{"repository", source.repository.has_value()
                            ? nlohmann::json(PathUtf8(*source.repository))
                            : nlohmann::json()},
         {"path", source.path.has_value() ? nlohmann::json(*source.path)
                                          : nlohmann::json()}});
  }
  return result;
}

nlohmann::json ConfigurationJson(
    const AnalyzeArguments& arguments, std::string_view requested_model,
    const std::vector<llmcc::RulesSource>& rules,
    const llmcc::ModelIdentity* identity = nullptr) {
  const llmcc::ScoringSettings& scoring = arguments.scoring;
  const bool percentile = scoring.tau_percentile.has_value();
  const llmcc::EffectiveTau tau =
      llmcc::ResolveTau(scoring, ModelRequestOf(arguments), identity);
  const char* effective_reducer =
      scoring.entropy_reduction == llmcc::EntropyReduction::kDevice ? "device"
                                                                    : "host";
  nlohmann::json configuration = {
      {"type", "configuration"},
      {"analysis_version", llmcc::kAnalysisVersion},
      {"hierarchy_mode",
       scoring.hierarchy_mode == llmcc::HierarchyMode::kStructural
           ? "structural"
           : "reference"},
      {"language", arguments.language_name},
      // Headers are always discovered; kept for configuration consumers.
      {"include_headers", true},
      {"no_ignore", arguments.no_ignore},
      {"rules", RulesJson(rules)},
      {"progress", arguments.progress},
      {"no_download", arguments.no_download},
      {"model", requested_model},
      {"context", scoring.context},
      {"window_policy", "fixed-half-overlap"},
      {"reference_context_tokens", llmcc::kDefaultContextSize},
      {"batch_size", scoring.batch_size},
      {"entropy_reduction",
       llmcc::EntropyReductionName(scoring.entropy_reduction)},
      {"effective_entropy_reducer", effective_reducer},
      {"flash_attn", llmcc::FlashAttentionName(scoring.flash_attention)},
      {"effective_flash_attn",
       scoring.flash_attention == llmcc::FlashAttention::kAuto &&
               scoring.kv_cache_type != llmcc::KvCacheType::kF16
           ? "on"
           : llmcc::FlashAttentionName(scoring.flash_attention)},
      {"kv_cache_type", llmcc::KvCacheTypeName(scoring.kv_cache_type)},
      {"effective_kv_cache_type",
       llmcc::KvCacheTypeName(scoring.kv_cache_type)},
      {"kv_offload", scoring.kv_offload ? "on" : "off"},
      {"effective_kv_offload", scoring.kv_offload ? "on" : "off"},
      {"backend_diagnostics", arguments.backend_diagnostics},
      {"score_mode", arguments.score_mode},
      {"tau_rule", percentile ? "percentile" : "absolute"},
      {"tau",
       tau.value.has_value() ? nlohmann::json(*tau.value) : nlohmann::json()},
      {"tau_source", tau.source},
      {"hotspots", arguments.hotspots},
      {"tau_percentile",
       percentile ? nlohmann::json(*scoring.tau_percentile) : nlohmann::json()},
      {"alpha", scoring.alpha},
      {"backend", llmcc::RequestedBackendCacheIdentity(scoring)},
      {"gpu_layers", scoring.gpu_layers},
      {"inference_abi", llmcc::InferenceAbi()},
      {"cache",
       {{"enabled",
         !arguments.no_cache && !scoring.backend_directory.has_value()},
        {"scope", "user"},
        {"version", 2},
        {"namespace", "v2/entropy"},
        {"limit_bytes", llmcc::kEntropyCacheLimit},
        {"retention_seconds", llmcc::kEntropyCacheMaxAgeSeconds}}}};
  configuration["cache"]["entries"] = nullptr;
  configuration["cache"]["bytes"] = nullptr;
  if (configuration["cache"]["enabled"].get<bool>()) {
    try {
      const auto status = llmcc::GetEntropyCacheStatus(false);
      configuration["cache"]["directory"] = PathUtf8(status.directory);
      configuration["cache"]["entries"] = status.entries;
      configuration["cache"]["bytes"] = status.bytes;
    } catch (const std::exception&) {
      // Cache bookkeeping must not prevent analysis or a validated hit.
    }
  }
  if (identity != nullptr) {
    configuration["model"] = PathUtf8(identity->canonical_path);
    configuration["model_size"] = identity->size;
    configuration["model_modification_time"] = identity->modification_time;
    if (!identity->content_digest.empty()) {
      configuration["model_sha256"] = identity->content_digest;
    }
    configuration["backend"] = identity->backend;
    configuration["effective_entropy_reducer"] = identity->effective_reducer;
    configuration["effective_kv_cache_type"] = identity->kv_cache_type;
    configuration["effective_kv_offload"] = identity->kv_offload ? "on" : "off";
  }
  return configuration;
}

nlohmann::json FunctionJson(const llmcc::FunctionScore& function,
                            std::string_view score_mode) {
  using llmcc::ScoreJson;
  return {{"name", function.name},
          {"start_line", function.start_line},
          {"end_line", function.end_line},
          {"score", ScoreJson(function.metrics, score_mode)},
          {"lmcc", function.metrics.lmcc},
          {"lmcc_per_token", ScoreJson(function.metrics, "lmcc")},
          {"density", ScoreJson(function.metrics, "density")},
          {"mean_entropy", ScoreJson(function.metrics, "mean")},
          {"token_count", function.metrics.token_count}};
}

std::string FormatNumber(double value) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::fixed << std::setprecision(3) << value;
  return output.str();
}

std::string FormatRaw(double value) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::fixed << std::setprecision(1) << value;
  return output.str();
}

std::string FormatScore(const llmcc::Metrics& metrics,
                        std::string_view score_mode) {
  if (score_mode == "raw") {
    return FormatRaw(metrics.lmcc);
  }
  if (metrics.token_count == 0) {
    return "null";
  }
  if (score_mode == "density") {
    return FormatNumber(metrics.density);
  }
  if (score_mode == "mean") {
    return FormatNumber(metrics.mean_entropy);
  }
  return FormatNumber(metrics.lmcc_per_token);
}

std::string_view ScoreLabel(std::string_view score_mode) {
  if (score_mode == "raw") {
    return "LM-CC";
  }
  if (score_mode == "density") {
    return "density";
  }
  if (score_mode == "mean") {
    return "mean entropy";
  }
  return "lmcc/token";
}

std::string_view SourceLine(std::string_view contents,
                            const std::vector<std::size_t>& line_starts,
                            std::size_t line) {
  if (line == 0 || line > line_starts.size()) {
    return {};
  }
  const std::size_t start = line_starts[line - 1];
  std::size_t end =
      line < line_starts.size() ? line_starts[line] - 1 : contents.size();
  if (end > start && contents[end - 1] == '\r') {
    --end;
  }
  return contents.substr(start, end - start);
}

void PrintFileText(const llmcc::DiscoveredSource& source,
                   std::string_view contents,
                   const llmcc::FileAnalysisResult& result,
                   std::string_view score_mode) {
  const llmcc::Metrics& metrics = result.analysis.metrics;
  std::cout << TerminalSafe(PathUtf8(source.path)) << "   score "
            << FormatScore(metrics, score_mode) << " ("
            << ScoreLabel(score_mode) << ")   density ";
  if (metrics.token_count == 0) {
    std::cout << "null   mean null";
  } else {
    std::cout << FormatNumber(metrics.density) << "   mean "
              << FormatNumber(metrics.mean_entropy);
  }
  std::cout << "   tokens " << metrics.token_count << '\n';
  const auto& scoring = result.scoring;
  const double source_context_ratio =
      static_cast<double>(scoring.source_tokens) / llmcc::kDefaultContextSize;
  if (source_context_ratio > 1.0) {
    std::cout << "  context: " << scoring.source_tokens
              << " source tokens; fixed half-overlap inference windows "
              << scoring.window_count << " x "
              << scoring.inference_context_tokens << " (stride "
              << scoring.window_stride_tokens << "); default "
              << llmcc::kDefaultContextSize << "-token context overflow "
              << FormatNumber(source_context_ratio - 1.0) << '\n';
  }
  for (const llmcc::FunctionScore& function : result.functions) {
    std::cout << "  fn " << std::left << std::setw(18)
              << TerminalSafe(function.name) << std::right << " L"
              << function.start_line << "-L" << function.end_line << "   "
              << FormatScore(function.metrics, score_mode) << '\n';
  }
  if (!result.hotspots.empty()) {
    const auto line_starts = llmcc::LineStarts(contents);
    std::cout << "  hotspots:\n";
    for (const llmcc::Hotspot& hotspot : result.hotspots) {
      std::cout << "    L" << hotspot.line
                << "  H=" << FormatNumber(hotspot.max_entropy) << "  | "
                << TerminalSafe(SourceLine(contents, line_starts, hotspot.line))
                << '\n';
    }
  }
  std::cout << '\n';
}

void PrintTotalsText(const llmcc::MetricTotals& totals,
                     std::string_view score_mode) {
  std::cout << "totals   score "
            << FormatScore(llmcc::TotalMetrics(totals), score_mode) << " ("
            << ScoreLabel(score_mode) << ")   files " << totals.analyzed << '/'
            << totals.discovered << "   tokens " << totals.token_count;
  if (score_mode == "raw" && totals.analyzed != 0) {
    std::cout << "   mean/file "
              << FormatRaw(totals.llm_cc /
                           static_cast<double>(totals.analyzed));
  }
  std::cout << '\n';
}

nlohmann::json FileJson(const llmcc::DiscoveredSource& source,
                        const llmcc::FileAnalysisResult& result,
                        const AnalyzeArguments& arguments) {
  const std::string language(llmcc::LanguageName(source.language));
  nlohmann::json event = llmcc::AnalysisJson(result.analysis);
  event["type"] = "file";
  event["path"] = PathUtf8(source.path);
  event["language"] = language;
  event["category"] = llmcc::CategoryName(source.category);
  event["entropy_cache_hit"] = result.entropy_cache_hit;
  event["score"] =
      llmcc::ScoreJson(result.analysis.metrics, arguments.score_mode);
  event["score_mode"] = arguments.score_mode;
  const auto& scoring = result.scoring;
  const double source_context_ratio =
      static_cast<double>(scoring.source_tokens) / llmcc::kDefaultContextSize;
  event["source_token_count"] = scoring.source_tokens;
  event["inference_context_tokens"] = scoring.inference_context_tokens;
  event["window_policy"] = "fixed-half-overlap";
  event["window_stride_tokens"] = scoring.window_stride_tokens;
  event["inference_window_count"] = scoring.window_count;
  event["reference_context_tokens"] = llmcc::kDefaultContextSize;
  event["source_context_ratio"] = source_context_ratio;
  event["reference_context_overflow"] =
      std::max(0.0, source_context_ratio - 1.0);
  event["context_overflow_tokens"] =
      scoring.source_tokens > llmcc::kDefaultContextSize
          ? scoring.source_tokens - llmcc::kDefaultContextSize
          : 0;
  event["functions"] = nlohmann::json::array();
  for (const llmcc::FunctionScore& function : result.functions) {
    event["functions"].push_back(FunctionJson(function, arguments.score_mode));
  }
  if (arguments.hotspots != 0) {
    event["hotspots"] = nlohmann::json::array();
    for (const llmcc::Hotspot& hotspot : result.hotspots) {
      event["hotspots"].push_back({{"line", hotspot.line},
                                   {"max_entropy", hotspot.max_entropy},
                                   {"mean_entropy", hotspot.mean_entropy},
                                   {"high_tokens", hotspot.high_tokens}});
    }
  }
  return event;
}

void ReportFileError(const llmcc::DiscoveredSource& source,
                     const std::exception& error, bool text, bool fatal) {
  if (text) {
    std::cerr << "error: " << TerminalSafe(PathUtf8(source.path)) << ": "
              << TerminalSafe(error.what()) << '\n';
    return;
  }
  nlohmann::json event = {{"type", "error"},
                          {"path", PathUtf8(source.path)},
                          {"language", llmcc::LanguageName(source.language)},
                          {"message", error.what()}};
  if (fatal) {
    event["fatal"] = true;
  }
  Emit(event);
}

llmcc::MetricTotals& LanguageTotals(llmcc::GroupTotals& groups,
                                    const llmcc::DiscoveredSource& source) {
  return groups.languages[std::string(llmcc::LanguageName(source.language))];
}

llmcc::MetricTotals& CategoryTotals(llmcc::GroupTotals& groups,
                                    const llmcc::DiscoveredSource& source) {
  return groups.categories[std::string(llmcc::CategoryName(source.category))];
}

void CountDiscovered(llmcc::GroupTotals& groups,
                     const llmcc::DiscoveredSource& source) {
  ++LanguageTotals(groups, source).discovered;
  ++CategoryTotals(groups, source).discovered;
}

void CountFailed(llmcc::GroupTotals& groups,
                 const llmcc::DiscoveredSource& source) {
  ++LanguageTotals(groups, source).failed;
  ++CategoryTotals(groups, source).failed;
}

int RunAnalyze(const AnalyzeArguments& arguments) {
  const llmcc::ScoringSettings& scoring = arguments.scoring;
  llmcc::CheckDeviceReduction(scoring);
  const bool text = arguments.format == "text";
  ProgressReporter progress(arguments.progress);
  llmcc::CliSession session(progress, arguments.assume_yes,
                            arguments.no_download);
  progress.Phase("discovering sources");
  const auto warning = [&](std::string_view message) {
    if (text) {
      std::cerr << "warning: " << TerminalSafe(message) << '\n';
    } else {
      Emit({{"type", "warning"}, {"message", message}});
    }
  };
  llmcc::DiscoveryResult discovery = llmcc::DiscoverSources(
      arguments.sources,
      {.language = arguments.language, .no_ignore = arguments.no_ignore});
  const std::string requested_model =
      arguments.model.has_value() ? PathUtf8(*arguments.model)
                                  : arguments.model_name.value_or("default");
  if (!text) {
    Emit({{"type", "start"},
          {"discovered", discovery.sources.size()},
          {"model", requested_model}});
  }

  if (discovery.sources.empty()) {
    if (!text) {
      Emit(ConfigurationJson(arguments, requested_model, discovery.rules));
    }
    for (const auto& message : discovery.warnings) {
      warning(message);
    }
    warning("no eligible source files were discovered");
    if (text) {
      PrintTotalsText({}, arguments.score_mode);
    } else {
      Emit(llmcc::TotalsJson({}, {}, false, arguments.score_mode,
                             scoring.hierarchy_mode));
    }
    return 0;
  }

  // Preflight known-oversize regular files before resolving a model. Keep
  // failures per file so one pathological input cannot hide later results;
  // the bounded read below remains authoritative for growing streams.
  std::map<std::filesystem::path, std::string> preflight_errors;
  for (const auto& source : discovery.sources) {
    try {
      llmcc::CheckSourceFileSize(source.path);
    } catch (const std::length_error& error) {
      preflight_errors.emplace(source.path, error.what());
    }
  }

  // Do not resolve a model when every discovered input has already been
  // refused. This retains the regular per-file error/totals contract while
  // making an oversize-only invocation prompt and independent of model state.
  if (preflight_errors.size() == discovery.sources.size()) {
    if (!text) {
      Emit(ConfigurationJson(arguments, requested_model, discovery.rules));
    }
    for (const auto& message : discovery.warnings) {
      warning(message);
    }
    llmcc::MetricTotals totals;
    totals.discovered = discovery.sources.size();
    llmcc::GroupTotals groups;
    std::size_t file_index = 0;
    for (const auto& source : discovery.sources) {
      const std::string language(llmcc::LanguageName(source.language));
      CountDiscovered(groups, source);
      progress.StartFile(++file_index, discovery.sources.size(), source.path);
      if (!text) {
        Emit({{"type", "file_start"},
              {"path", PathUtf8(source.path)},
              {"language", language}});
      }
      progress.FailFile();
      ++totals.failed;
      CountFailed(groups, source);
      ReportFileError(source,
                      std::runtime_error(preflight_errors.at(source.path)),
                      text, false);
    }
    if (text) {
      PrintTotalsText(totals, arguments.score_mode);
    } else {
      Emit(llmcc::TotalsJson(totals, groups, false, arguments.score_mode,
                             scoring.hierarchy_mode));
    }
    return 1;
  }

  llmcc::ScorerSession scorer = llmcc::OpenScorerSession(
      {.settings = scoring,
       .model = ModelRequestOf(arguments),
       .no_download = arguments.no_download,
       .no_cache = arguments.no_cache,
       .backend_diagnostics = arguments.backend_diagnostics,
       .hotspots = arguments.hotspots},
      progress);
  if (!text) {
    Emit(ConfigurationJson(arguments, requested_model, discovery.rules,
                           &scorer.identity));
  }
  for (const auto& message : discovery.warnings) {
    warning(message);
  }
  if (scorer.tau.source == std::string_view("model-digest-mismatch")) {
    warning("the cached model does not match the registered " +
            std::string(requested_model) +
            "; using the paper threshold instead of its calibrated tau");
  }
  if (!arguments.no_cache && scoring.backend_directory.has_value()) {
    warning("entropy caching is disabled for custom backend directories");
  }
  if (scorer.entropy_cache) {
    try {
      llmcc::CheckEntropyCacheAvailability();
    } catch (const std::exception& error) {
      warning("entropy cache is unavailable: " + std::string(error.what()));
    }
  }
  llmcc::ProjectAnalyzer& analyzer = *scorer.analyzer;

  llmcc::MetricTotals totals;
  totals.discovered = discovery.sources.size();
  llmcc::GroupTotals groups;
  for (const auto& source : discovery.sources) {
    CountDiscovered(groups, source);
  }
  bool fatal = false;
  std::size_t file_index = 0;
  for (const auto& source : discovery.sources) {
    progress.StartFile(++file_index, discovery.sources.size(), source.path);
    const std::string language(llmcc::LanguageName(source.language));
    if (!text) {
      Emit({{"type", "file_start"},
            {"path", PathUtf8(source.path)},
            {"language", language}});
    }
    try {
      if (const auto preflight = preflight_errors.find(source.path);
          preflight != preflight_errors.end()) {
        throw std::runtime_error(preflight->second);
      }
      progress.Phase("reading source");
      const std::string contents = ReadFile(source.path);
      if (contents.size() > 1024 * 1024) {
        progress.Phase("scoring full file " + PathUtf8(source.path) + " (" +
                       std::to_string(contents.size()) +
                       " bytes) with overlapping inference windows");
      }
      auto result = analyzer.AnalyzeFile(source, contents);
      progress.Phase("writing output");
      if (text) {
        PrintFileText(source, contents, result, arguments.score_mode);
      } else {
        Emit(FileJson(source, result, arguments));
      }
      progress.FinishFile(result.entropy_cache_hit);
      ++totals.analyzed;
      llmcc::Accumulate(result.analysis, totals);
      for (llmcc::MetricTotals* group :
           {&LanguageTotals(groups, source), &CategoryTotals(groups, source)}) {
        ++group->analyzed;
        llmcc::Accumulate(result.analysis, *group);
      }
    } catch (const llmcc::GpuRecoverableError&) {
      progress.FailFile();
      throw;
    } catch (const llmcc::ScorerInitializationError& error) {
      progress.FailFile();
      ++totals.failed;
      CountFailed(groups, source);
      ReportFileError(source, error, text, true);
      fatal = true;
    } catch (const std::exception& error) {
      progress.FailFile();
      ++totals.failed;
      CountFailed(groups, source);
      ReportFileError(source, error, text, false);
    }
  }

  if (text) {
    PrintTotalsText(totals, arguments.score_mode);
  } else {
    Emit(llmcc::TotalsJson(totals, groups, fatal, arguments.score_mode,
                           scoring.hierarchy_mode));
  }
  if (fatal) {
    return 2;
  }
  return totals.failed == 0 ? 0 : 1;
}

}  // namespace

int Main(int argc, char** argv) {
  try {
    int result = 0;
    if (argc > 1 && std::string_view(argv[1]) == "score") {
      result = llmcc::RunScoreCommand(argc - 1, argv + 1);
    } else if (argc > 1 && std::string_view(argv[1]) == "models") {
      try {
        result = RunModels(argc, argv);
      } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
      }
    } else if (argc > 1 && std::string_view(argv[1]) == "backends") {
      result = RunBackends(argc, argv);
    } else if (argc > 1 && std::string_view(argv[1]) == "cache") {
      result = RunCache(argc, argv);
    } else if (argc > 1 && std::string_view(argv[1]) == "rules") {
      result = llmcc::RunRulesCommand(argc - 1, argv + 1);
    } else {
      const auto arguments = ParseAnalyzeArguments(argc, argv);
      try {
        result = RunAnalyze(arguments);
      } catch (const llmcc::GpuRecoverableError& error) {
        throw std::runtime_error(std::string(error.what()) + "\n" +
                                 llmcc::CpuRecoveryCommand(argc, argv) + "\n" +
                                 llmcc::SmallerModelGuidance());
      }
    }
    std::cout.flush();
    if (!std::cout) {
      throw std::runtime_error("failed to write output");
    }
    return result;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** wide_argv) {
  std::vector<std::string> encoded;
  encoded.reserve(argc);
  for (int index = 0; index < argc; ++index) {
    encoded.push_back(PathUtf8(std::filesystem::path(wide_argv[index])));
  }
  std::vector<char*> argv;
  argv.reserve(argc);
  for (std::string& argument : encoded) {
    argv.push_back(argument.data());
  }
  return Main(argc, argv.data());
}
#else
int main(int argc, char** argv) { return Main(argc, argv); }
#endif
