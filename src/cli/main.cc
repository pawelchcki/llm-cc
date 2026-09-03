#include <unistd.h>

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
#include <type_traits>
#include <vector>

#include "generated/version.h"
#include "src/analyze.h"
#include "src/cache.h"
#include "src/download.h"
#include "src/entropy_cache.h"
#include "src/jsonl.h"
#include "src/lang.h"
#include "src/project.h"
#include "src/score_cmd.h"

namespace {

struct AnalyzeArguments {
  std::vector<std::filesystem::path> sources;
  std::optional<llmcc::Language> language;
  std::string language_name = "auto";
  std::optional<std::filesystem::path> model;
  bool no_download = false;
  bool include_headers = false;
  bool no_ignore = false;
  bool no_cache = false;
  std::int32_t gpu_layers = 0;
  llmcc::BackendKind backend = llmcc::BackendKind::kAuto;
  std::uint32_t context = llmcc::kDefaultContextSize;
  std::uint32_t batch_size = llmcc::kDefaultBatchSize;
  llmcc::EntropyReduction entropy_reduction = llmcc::EntropyReduction::kAuto;
  llmcc::HierarchyMode hierarchy_mode = llmcc::HierarchyMode::kStructural;
  std::optional<double> tau;
  std::optional<double> tau_percentile;
  double alpha = 0.8;
  std::size_t hotspots = 10;
  std::string score_mode = "lmcc";
  std::string format = "jsonl";
  std::string progress = "auto";
};

constexpr std::string_view kUsageBeforeContext =
    "Usage:\n"
    "  llm-cc PATH... [--lang "
    "auto|rust|c|cpp|java|python|go|javascript|csharp] [OPTIONS]\n"
    "  llm-cc score --model GGUF [--prompt TEXT | --file PATH] [OPTIONS]\n"
    "  llm-cc models list|remove FILE|path\n"
    "  llm-cc cache status|prune|clear [PATH] [--format text|json] "
    "[--legacy]\n\n"
    "Analysis options:\n"
    "  --lang NAME          infer per file with auto, or force every input to\n"
    "                       rust, c, cpp, java, python, go, javascript, or\n"
    "                       csharp (default: auto)\n"
    "  --include-headers     include headers during recursive discovery\n"
    "  --no-ignore           include ignored and generated source files\n"
    "  --no-cache            disable repository-scoped entropy caching\n"
    "  --no-download         do not fetch the default model\n"
    "  --model GGUF          llama.cpp-compatible model\n"
    "  --score lmcc|density|mean  headline score mode (default: lmcc)\n"
    "  --hierarchy structural|reference  hierarchy contract (default: "
    "structural)\n"
    "  --tau N               absolute entropy threshold in nats (default: "
    "0.67)\n"
    "  --gpu-layers N        transformer layers to offload (-1 means all)\n"
    "  --backend NAME        auto, cpu, cuda, or rocm (default: auto)\n"
    "  --batch-size N        decode rows per batch (default: 64)\n"
    "  --entropy-reduction M auto, host, or device (default: auto)\n"
    "  --context N           maximum input tokens (default: ";

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
  Number result{};
  bool valid = false;
  if constexpr (std::is_floating_point_v<Number>) {
    std::istringstream input{std::string(value)};
    input.imbue(std::locale::classic());
    input >> std::noskipws >> result;
    valid = input && input.eof();
  } else {
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), result);
    valid = error == std::errc{} && end == value.data() + value.size();
  }
  if (!valid) {
    Usage(std::string(option) + " expects a number, got '" +
          std::string(value) + "'");
  }
  return result;
}

void SetAnalyzeOption(AnalyzeArguments& arguments, std::string_view option,
                      std::string_view value) {
  if (option == "--lang") {
    arguments.language_name = value;
    if (value == "auto") {
      arguments.language.reset();
    } else {
      arguments.language = llmcc::ParseLanguage(value);
      arguments.language_name = llmcc::LanguageName(*arguments.language);
    }
  } else if (option == "--model") {
    arguments.model = value;
  } else if (option == "--gpu-layers") {
    arguments.gpu_layers = ParseNumber<std::int32_t>(option, value);
    if (arguments.gpu_layers < -1) {
      Usage("--gpu-layers must be -1 or greater");
    }
  } else if (option == "--backend") {
    try {
      arguments.backend = llmcc::ParseBackend(value);
    } catch (const std::invalid_argument& error) {
      Usage(error.what());
    }
  } else if (option == "--context") {
    arguments.context = ParseNumber<std::uint32_t>(option, value);
    if (arguments.context == 0) {
      Usage("--context must be positive");
    }
  } else if (option == "--batch-size") {
    arguments.batch_size = ParseNumber<std::uint32_t>(option, value);
    if (arguments.batch_size == 0) {
      Usage("--batch-size must be positive");
    }
  } else if (option == "--entropy-reduction") {
    if (value == "auto") {
      arguments.entropy_reduction = llmcc::EntropyReduction::kAuto;
    } else if (value == "host") {
      arguments.entropy_reduction = llmcc::EntropyReduction::kHost;
    } else if (value == "device") {
      arguments.entropy_reduction = llmcc::EntropyReduction::kDevice;
    } else {
      Usage("--entropy-reduction expects auto, host, or device");
    }
  } else if (option == "--hierarchy") {
    if (value == "structural") {
      arguments.hierarchy_mode = llmcc::HierarchyMode::kStructural;
    } else if (value == "reference") {
      arguments.hierarchy_mode = llmcc::HierarchyMode::kReference;
    } else {
      Usage("--hierarchy expects structural or reference");
    }
  } else if (option == "--tau-percentile") {
    arguments.tau_percentile = ParseNumber<double>(option, value);
  } else if (option == "--tau") {
    arguments.tau = ParseNumber<double>(option, value);
  } else if (option == "--hotspots") {
    arguments.hotspots = ParseNumber<std::size_t>(option, value);
  } else if (option == "--score") {
    arguments.score_mode = value;
    if (arguments.score_mode != "lmcc" && arguments.score_mode != "density" &&
        arguments.score_mode != "mean") {
      Usage("--score expects lmcc, density, or mean");
    }
  } else if (option == "--format") {
    arguments.format = value == "json" ? "jsonl" : std::string(value);
    if (arguments.format != "jsonl" && arguments.format != "text") {
      Usage("--format expects jsonl, json, or text");
    }
  } else if (option == "--progress") {
    arguments.progress = value;
    if (arguments.progress != "auto" && arguments.progress != "always" &&
        arguments.progress != "never") {
      Usage("--progress expects auto, always, or never");
    }
  } else if (option == "--alpha") {
    arguments.alpha = ParseNumber<double>(option, value);
  } else {
    Usage("unknown option: " + std::string(option));
  }
}

AnalyzeArguments ParseAnalyzeArguments(int argc, char** argv) {
  AnalyzeArguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (option == "-h" || option == "--help") {
      Usage();
    }
    if (option == "-V" || option == "--version") {
      std::cout << "llm-cc " << LLM_CC_VERSION << '\n';
      std::exit(0);
    }
    if (option == "--no-download") {
      arguments.no_download = true;
      continue;
    }
    if (option == "--include-headers") {
      arguments.include_headers = true;
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
    if (!option.starts_with('-')) {
      arguments.sources.emplace_back(option);
      continue;
    }
    if (index + 1 >= argc) {
      Usage(std::string(option) + " requires a value");
    }
    SetAnalyzeOption(arguments, option, argv[++index]);
  }
  if (arguments.sources.empty()) {
    Usage("at least one source path is required unless a subcommand is used");
  }
  if (arguments.tau.has_value() && arguments.tau_percentile.has_value()) {
    Usage("--tau and --tau-percentile are mutually exclusive");
  }
  if (arguments.tau.has_value() &&
      (!std::isfinite(*arguments.tau) || *arguments.tau < 0.0)) {
    Usage("--tau must be finite and non-negative");
  }
  if (arguments.tau_percentile.has_value() &&
      (!std::isfinite(*arguments.tau_percentile) ||
       *arguments.tau_percentile < 0.0 || *arguments.tau_percentile > 100.0)) {
    Usage("--tau-percentile must be finite and between 0 and 100");
  }
  if (!std::isfinite(arguments.alpha) || arguments.alpha < 0.0 ||
      arguments.alpha > 1.0) {
    Usage("--alpha must be finite and between 0 and 1");
  }
  try {
    static_cast<void>(
        llmcc::SelectBackend(arguments.backend, arguments.gpu_layers, {}));
  } catch (const std::invalid_argument& error) {
    Usage(error.what());
  } catch (const std::runtime_error&) {
    // Device availability is checked after the selected plugins are loaded.
  }
  return arguments;
}

std::string ReadFile(const std::filesystem::path& path) {
  using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
  File input(std::fopen(path.c_str(), "rb"), std::fclose);
  if (!input) {
    throw std::runtime_error("failed to read " + path.string());
  }
  std::string contents;
  std::array<char, std::size_t{64} * 1024> buffer{};
  for (;;) {
    const std::size_t count =
        std::fread(buffer.data(), 1, buffer.size(), input.get());
    contents.append(buffer.data(), count);
    if (count == buffer.size()) {
      continue;
    }
    if (std::ferror(input.get()) != 0) {
      throw std::runtime_error("failed while reading " + path.string());
    }
    break;
  }
  return contents;
}

void Emit(const nlohmann::json& event) {
  std::cout << event.dump() << '\n' << std::flush;
  if (!std::cout) {
    throw std::runtime_error("failed to write output");
  }
}

std::string TerminalSafe(std::string_view text);

class ProgressReporter {
 public:
  explicit ProgressReporter(std::string_view mode)
      : enabled_(mode == "always" ||
                 (mode == "auto" && isatty(STDERR_FILENO) != 0)) {}

  void Phase(std::string_view message) const {
    if (enabled_) {
      std::cerr << "llm-cc: " << message << '\n' << std::flush;
    }
  }

  void StartFile(std::size_t index, std::size_t total,
                 const std::filesystem::path& path) {
    file_index_ = index;
    file_total_ = total;
    path_ = path.string();
    started_ = std::chrono::steady_clock::now();
    last_update_ = {};
    if (enabled_) {
      std::cerr << "llm-cc: [" << index << '/' << total << "] analyzing "
                << TerminalSafe(path_) << '\n'
                << std::flush;
    }
  }

  void Tokens(std::size_t completed, std::size_t total) {
    if (!enabled_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (completed != 0 && completed != total && last_update_ != TimePoint{} &&
        now - last_update_ < std::chrono::seconds(1)) {
      return;
    }
    last_update_ = now;
    std::cerr << "llm-cc: [" << file_index_ << '/' << file_total_ << "] "
              << completed << '/' << total << " tokens\n"
              << std::flush;
  }

  void FinishFile(bool cache_hit) const {
    if (!enabled_) {
      return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_);
    std::cerr << "llm-cc: [" << file_index_ << '/' << file_total_
              << "] complete cache=" << (cache_hit ? "hit" : "miss")
              << " elapsed_ms=" << elapsed.count() << '\n'
              << std::flush;
  }

  void FailFile() const {
    if (enabled_) {
      std::cerr << "llm-cc: [" << file_index_ << '/' << file_total_
                << "] failed\n"
                << std::flush;
    }
  }

 private:
  using TimePoint = std::chrono::steady_clock::time_point;
  bool enabled_;
  std::size_t file_index_ = 0;
  std::size_t file_total_ = 0;
  std::string path_;
  TimePoint started_{};
  TimePoint last_update_{};
};

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
  if (action == "path" && argc == 3) {
    std::cout << cache_dir.string() << '\n';
    return 0;
  }
  if (action == "remove" && argc == 4) {
    llmcc::RemoveModel(cache_dir, argv[3]);
    return 0;
  }
  Usage("invalid models command");
}

struct CacheArguments {
  std::string_view action;
  std::filesystem::path path = std::filesystem::current_path();
  std::string_view format = "text";
  bool legacy = false;
};

CacheArguments ParseCacheArguments(int argc, char** argv) {
  if (argc < 3) {
    Usage("cache requires status, prune, or clear");
  }
  CacheArguments arguments{.action = argv[2]};
  bool path_set = false;
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
    } else if (!value.starts_with('-') && !path_set) {
      arguments.path = value;
      path_set = true;
    } else {
      Usage("invalid cache option: " + std::string(value));
    }
  }
  if (arguments.action != "status" && arguments.action != "prune" &&
      arguments.action != "clear") {
    Usage("cache requires status, prune, or clear");
  }
  if (arguments.legacy && arguments.action != "clear") {
    Usage("--legacy is only valid with cache clear");
  }
  return arguments;
}

int RunCache(int argc, char** argv) {
  const CacheArguments arguments = ParseCacheArguments(argc, argv);
  const auto repository = llmcc::FindGitRepository(arguments.path);
  if (!repository.has_value()) {
    throw std::invalid_argument(arguments.path.string() +
                                " is not inside a Git worktree");
  }
  if (arguments.action == "prune") {
    llmcc::PruneRepositoryCache(*repository);
  } else if (arguments.action == "clear") {
    llmcc::ClearRepositoryCache(*repository);
    if (arguments.legacy) {
      llmcc::ClearLegacyRepositoryCache(*repository);
    }
  }
  const auto status = llmcc::GetRepositoryCacheStatus(*repository);
  if (arguments.format == "json") {
    std::cout << nlohmann::json{{"repository", status.repository.string()},
                                {"directory", status.directory.string()},
                                {"storage_version", 1},
                                {"inference_abi", llmcc::InferenceAbi()},
                                {"entries", status.entries},
                                {"bytes", status.bytes},
                                {"legacy_directory",
                                 status.legacy_directory.string()},
                                {"legacy_entries", status.legacy_entries},
                                {"legacy_bytes", status.legacy_bytes},
                                {"entries_by_inference_abi",
                                 status.entries_by_inference_abi},
                                {"unknown_provenance_entries",
                                 status.unknown_provenance_entries},
                                {"malformed_entries", status.malformed_entries}}
                     .dump()
              << '\n';
  } else {
    std::cout << "repository: " << status.repository.string() << '\n'
              << "directory: " << status.directory.string() << '\n'
              << "storage version: 1\n"
              << "inference ABI: " << llmcc::InferenceAbi() << '\n'
              << "entries: " << status.entries << '\n'
              << "bytes: " << status.bytes << '\n'
              << "legacy directory: " << status.legacy_directory.string()
              << '\n'
              << "legacy entries: " << status.legacy_entries << '\n'
              << "legacy bytes: " << status.legacy_bytes << '\n'
              << "unknown provenance entries: "
              << status.unknown_provenance_entries << '\n'
              << "malformed entries: " << status.malformed_entries << '\n';
  }
  return 0;
}

class LlamaEntropyProvider : public llmcc::EntropyProvider {
 public:
  LlamaEntropyProvider(const std::filesystem::path& cache_dir,
                       const std::filesystem::path& model,
                       std::uint32_t context, std::int32_t gpu_layers,
                       llmcc::BackendKind backend, std::uint32_t batch_size,
                       llmcc::EntropyReduction entropy_reduction,
                       std::function<void(std::size_t, std::size_t)> progress)
      : scorer_((llmcc::MarkCachedModelUsed(cache_dir, model), model),
                {.context_size = context,
                 .gpu_layers = gpu_layers,
                 .backend = backend,
                 .batch_size = batch_size,
                 .entropy_reduction = entropy_reduction,
                 .progress = std::move(progress)}) {}

  std::vector<llmcc::EntropyRecord> Score(std::string_view source) override {
    return scorer_.ScoreRecords(source);
  }

 private:
  llmcc::EntropyScorer scorer_;
};

std::string BackendCacheIdentity(llmcc::BackendKind backend,
                                 std::int32_t gpu_layers) {
  if (gpu_layers == 0 && backend == llmcc::BackendKind::kCpu) {
    return "cpu";
  }
  return std::string(llmcc::BackendName(backend)) +
         "/gpu-layers=" + std::to_string(gpu_layers);
}

std::string RequestedBackendCacheIdentity(const AnalyzeArguments& arguments) {
  const llmcc::BackendKind backend =
      arguments.backend == llmcc::BackendKind::kAuto &&
              arguments.gpu_layers == 0
          ? llmcc::BackendKind::kCpu
          : arguments.backend;
  return BackendCacheIdentity(backend, arguments.gpu_layers);
}

struct MetricTotals {
  std::uint64_t discovered = 0;
  std::uint64_t analyzed = 0;
  std::uint64_t failed = 0;
  double llm_cc = 0.0;
  std::uint64_t total_branch = 0;
  std::uint64_t total_comp_level = 0;
  std::uint64_t token_count = 0;
  std::uint64_t high_entropy_tokens = 0;
  double entropy_sum = 0.0;
  double lmcc = 0.0;
};

nlohmann::json ScoreJson(const llmcc::Metrics& metrics,
                         std::string_view score_mode) {
  if (metrics.token_count == 0) {
    return nullptr;
  }
  if (score_mode == "density") {
    return metrics.density;
  }
  if (score_mode == "mean") {
    return metrics.mean_entropy;
  }
  return metrics.lmcc_per_token;
}

nlohmann::json TotalsMetricsJson(const MetricTotals& totals,
                                 std::string_view score_mode) {
  nlohmann::json lmcc_per_token = nullptr;
  nlohmann::json density = nullptr;
  nlohmann::json mean_entropy = nullptr;
  nlohmann::json score = nullptr;
  if (totals.token_count != 0) {
    const double token_count = static_cast<double>(totals.token_count);
    lmcc_per_token = totals.lmcc / token_count;
    density = static_cast<double>(totals.high_entropy_tokens) / token_count;
    mean_entropy = totals.entropy_sum / token_count;
    if (score_mode == "density") {
      score = density;
    } else if (score_mode == "mean") {
      score = mean_entropy;
    } else {
      score = lmcc_per_token;
    }
  }
  return {{"score", std::move(score)},
          {"lmcc_per_token", std::move(lmcc_per_token)},
          {"density", std::move(density)},
          {"mean_entropy", std::move(mean_entropy)},
          {"token_count", totals.token_count},
          {"high_entropy_tokens", totals.high_entropy_tokens}};
}

nlohmann::json TotalsJson(const MetricTotals& totals,
                          const std::map<std::string, MetricTotals>& languages,
                          bool fatal, std::string_view score_mode,
                          llmcc::HierarchyMode hierarchy_mode) {
  nlohmann::json language_json = nlohmann::json::object();
  for (const auto& [name, value] : languages) {
    nlohmann::json item = TotalsMetricsJson(value, score_mode);
    item.update({{"discovered", value.discovered},
                 {"analyzed", value.analyzed},
                 {"failed", value.failed},
                 {"llm_cc", value.llm_cc},
                 {"total_branch", value.total_branch},
                 {"total_comp_level", value.total_comp_level}});
    language_json[name] = std::move(item);
  }
  nlohmann::json result = TotalsMetricsJson(totals, score_mode);
  result.update(
      {{"type", "totals"},
       {"analysis_version", 2},
       {"hierarchy_mode", hierarchy_mode == llmcc::HierarchyMode::kStructural
                              ? "structural"
                              : "reference"},
       {"discovered", totals.discovered},
       {"analyzed", totals.analyzed},
       {"failed", totals.failed},
       {"llm_cc", totals.llm_cc},
       {"total_branch", totals.total_branch},
       {"total_comp_level", totals.total_comp_level},
       {"languages", std::move(language_json)},
       {"partial",
        fatal || totals.failed != 0 || totals.analyzed != totals.discovered}});
  return result;
}

nlohmann::json ConfigurationJson(
    const AnalyzeArguments& arguments, std::string_view requested_model,
    const llmcc::ModelIdentity* identity = nullptr) {
  const bool percentile = arguments.tau_percentile.has_value();
  nlohmann::json configured_tau = nullptr;
  if (!percentile) {
    configured_tau = arguments.tau.value_or(0.67);
  }
  nlohmann::json configured_percentile = nullptr;
  if (percentile) {
    configured_percentile = *arguments.tau_percentile;
  }
  nlohmann::json configuration = {
      {"type", "configuration"},
      {"analysis_version", 2},
      {"hierarchy_mode",
       arguments.hierarchy_mode == llmcc::HierarchyMode::kStructural
           ? "structural"
           : "reference"},
      {"language", arguments.language_name},
      {"include_headers", arguments.include_headers},
      {"no_ignore", arguments.no_ignore},
      {"progress", arguments.progress},
      {"no_download", arguments.no_download},
      {"model", requested_model},
      {"context", arguments.context},
      {"batch_size", arguments.batch_size},
      {"entropy_reduction",
       llmcc::EntropyReductionName(arguments.entropy_reduction)},
      {"effective_entropy_reducer", "host"},
      {"score_mode", arguments.score_mode},
      {"tau_rule", percentile ? "percentile" : "absolute"},
      {"tau", std::move(configured_tau)},
      {"hotspots", arguments.hotspots},
      {"tau_percentile", std::move(configured_percentile)},
      {"alpha", arguments.alpha},
      {"backend", RequestedBackendCacheIdentity(arguments)},
      {"gpu_layers", arguments.gpu_layers},
      {"inference_abi", llmcc::InferenceAbi()},
      {"cache",
       {{"enabled", !arguments.no_cache},
        {"version", 1},
        {"namespace", "external/v1/entropy"},
        {"limit_bytes", llmcc::kEntropyCacheLimit}}}};
  if (identity != nullptr) {
    configuration["model"] = identity->canonical_path.string();
    configuration["model_size"] = identity->size;
    configuration["model_modification_time"] = identity->modification_time;
    configuration["backend"] = identity->backend;
    configuration["effective_entropy_reducer"] = identity->effective_reducer;
  }
  return configuration;
}

void Accumulate(const llmcc::Analysis& analysis, MetricTotals& totals) {
  totals.llm_cc += analysis.llm_cc;
  totals.total_branch += analysis.total_branch;
  totals.total_comp_level += analysis.total_comp_level;
  if (analysis.metrics.token_count == 0) {
    return;
  }
  totals.token_count += analysis.metrics.token_count;
  totals.high_entropy_tokens += analysis.metrics.high_entropy_tokens;
  totals.entropy_sum += analysis.metrics.entropy_sum;
  totals.lmcc += analysis.metrics.lmcc;
}

nlohmann::json FunctionJson(const llmcc::FunctionScore& function,
                            std::string_view score_mode) {
  nlohmann::json lmcc_per_token = nullptr;
  nlohmann::json density = nullptr;
  nlohmann::json mean_entropy = nullptr;
  if (function.metrics.token_count != 0) {
    lmcc_per_token = function.metrics.lmcc_per_token;
    density = function.metrics.density;
    mean_entropy = function.metrics.mean_entropy;
  }
  return {{"name", function.name},
          {"start_line", function.start_line},
          {"end_line", function.end_line},
          {"score", ScoreJson(function.metrics, score_mode)},
          {"lmcc", function.metrics.lmcc},
          {"lmcc_per_token", std::move(lmcc_per_token)},
          {"density", std::move(density)},
          {"mean_entropy", std::move(mean_entropy)},
          {"token_count", function.metrics.token_count}};
}

std::string FormatNumber(double value) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::fixed << std::setprecision(3) << value;
  return output.str();
}

std::string FormatScore(const llmcc::Metrics& metrics,
                        std::string_view score_mode) {
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

std::string TerminalSafe(std::string_view text) {
  constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string safe;
  safe.reserve(text.size());
  const auto append_byte = [&](unsigned char byte) {
    safe.append("\\x");
    safe.push_back(kHex[byte >> 4]);
    safe.push_back(kHex[byte & 0x0f]);
  };
  const auto append_code_point = [&](std::uint32_t code_point) {
    safe.append("\\u");
    for (int shift = 12; shift >= 0; shift -= 4) {
      safe.push_back(kHex[(code_point >> shift) & 0x0f]);
    }
  };
  for (std::size_t index = 0; index < text.size();) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (byte < 0x20 || byte == 0x7f) {
      append_byte(byte);
      ++index;
      continue;
    }
    if (byte < 0x80) {
      safe.push_back(static_cast<char>(byte));
      ++index;
      continue;
    }
    std::size_t length = 0;
    if (byte >= 0xc2 && byte <= 0xdf) {
      length = 2;
    } else if (byte >= 0xe0 && byte <= 0xef) {
      length = 3;
    } else if (byte >= 0xf0 && byte <= 0xf4) {
      length = 4;
    }
    bool valid = length != 0 && index + length <= text.size();
    for (std::size_t offset = 1; valid && offset < length; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(text[index + offset]);
      valid = (continuation & 0xc0) == 0x80;
    }
    if (valid && length == 3) {
      const auto second = static_cast<unsigned char>(text[index + 1]);
      valid =
          (byte != 0xe0 || second >= 0xa0) && (byte != 0xed || second < 0xa0);
    } else if (valid && length == 4) {
      const auto second = static_cast<unsigned char>(text[index + 1]);
      valid =
          (byte != 0xf0 || second >= 0x90) && (byte != 0xf4 || second <= 0x8f);
    }
    if (!valid) {
      append_byte(byte);
      ++index;
      continue;
    }
    std::uint32_t code_point = byte & (length == 2   ? 0x1f
                                       : length == 3 ? 0x0f
                                                     : 0x07);
    for (std::size_t offset = 1; offset < length; ++offset) {
      code_point = (code_point << 6) |
                   (static_cast<unsigned char>(text[index + offset]) & 0x3f);
    }
    const bool bidi_control = code_point == 0x061c || code_point == 0x200e ||
                              code_point == 0x200f ||
                              (code_point >= 0x202a && code_point <= 0x202e) ||
                              (code_point >= 0x2066 && code_point <= 0x206f);
    if ((code_point >= 0x80 && code_point <= 0x9f) || bidi_control) {
      append_code_point(code_point);
      index += length;
      continue;
    }
    safe.append(text.substr(index, length));
    index += length;
  }
  return safe;
}

void PrintFileText(const llmcc::DiscoveredSource& source,
                   std::string_view contents,
                   const llmcc::FileAnalysisResult& result,
                   std::string_view score_mode) {
  const llmcc::Metrics& metrics = result.analysis.metrics;
  std::cout << TerminalSafe(source.path.string()) << "   score "
            << FormatScore(metrics, score_mode) << " ("
            << ScoreLabel(score_mode) << ")   density ";
  if (metrics.token_count == 0) {
    std::cout << "null   mean null";
  } else {
    std::cout << FormatNumber(metrics.density) << "   mean "
              << FormatNumber(metrics.mean_entropy);
  }
  std::cout << "   tokens " << metrics.token_count << '\n';
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

void PrintTotalsText(const MetricTotals& totals, std::string_view score_mode) {
  std::cout << "totals   score ";
  if (totals.token_count == 0) {
    std::cout << "null";
  } else {
    llmcc::Metrics metrics{
        .token_count = totals.token_count,
        .high_entropy_tokens = totals.high_entropy_tokens,
        .entropy_sum = totals.entropy_sum,
        .lmcc = totals.lmcc,
        .lmcc_per_token = totals.lmcc / static_cast<double>(totals.token_count),
        .density = static_cast<double>(totals.high_entropy_tokens) /
                   static_cast<double>(totals.token_count),
        .mean_entropy =
            totals.entropy_sum / static_cast<double>(totals.token_count)};
    std::cout << FormatScore(metrics, score_mode);
  }
  std::cout << " (" << ScoreLabel(score_mode) << ")   files " << totals.analyzed
            << '/' << totals.discovered << "   tokens " << totals.token_count
            << '\n';
}

int RunAnalyze(const AnalyzeArguments& arguments) {
  const bool text = arguments.format == "text";
  ProgressReporter progress(arguments.progress);
  progress.Phase("discovering sources");
  const auto warning = [&](std::string_view message) {
    if (text) {
      std::cerr << "warning: " << TerminalSafe(message) << '\n';
    } else {
      Emit({{"type", "warning"}, {"message", message}});
    }
  };
  llmcc::DiscoveryResult discovery = llmcc::DiscoverSources(
      arguments.sources, {.language = arguments.language,
                          .include_headers = arguments.include_headers,
                          .no_ignore = arguments.no_ignore});
  const std::string requested_model =
      arguments.model.has_value() ? arguments.model->string() : "default";
  if (!text) {
    Emit({{"type", "start"},
          {"discovered", discovery.sources.size()},
          {"model", requested_model}});
  }

  if (discovery.sources.empty()) {
    if (!text) {
      Emit(ConfigurationJson(arguments, requested_model));
    }
    for (const auto& message : discovery.warnings) {
      warning(message);
    }
    warning("no eligible source files were discovered");
    if (text) {
      PrintTotalsText({}, arguments.score_mode);
    } else {
      Emit(TotalsJson({}, {}, false, arguments.score_mode,
                      arguments.hierarchy_mode));
    }
    return 0;
  }

  const std::filesystem::path model_cache = llmcc::CacheDir();
  progress.Phase("resolving model");
  const auto resolved_model = llmcc::ResolveModel(
      arguments.model, arguments.no_download, std::filesystem::current_path(),
      model_cache, llmcc::DownloadDefaultModel);
  progress.Phase("selecting inference backend");
  const llmcc::BackendKind resolved_backend = [&]() {
    if (arguments.gpu_layers == 0) {
      return llmcc::BackendKind::kCpu;
    }
    llmcc::BackendRuntime runtime(arguments.backend, arguments.gpu_layers);
    return runtime.selected();
  }();
  const bool device_available = arguments.gpu_layers != 0 &&
                                (resolved_backend != llmcc::BackendKind::kCpu ||
                                 llmcc::CompiledBackend() == "metal");
  if (arguments.entropy_reduction == llmcc::EntropyReduction::kDevice &&
      !device_available) {
    throw std::invalid_argument(
        "device entropy reduction requires GPU execution");
  }
  const bool device_reduction =
      arguments.entropy_reduction == llmcc::EntropyReduction::kDevice ||
      (arguments.entropy_reduction == llmcc::EntropyReduction::kAuto &&
       device_available);
  const std::string backend_identity =
      BackendCacheIdentity(resolved_backend, arguments.gpu_layers);
  const auto identity = llmcc::InspectModel(
      resolved_model, llmcc::InferenceAbi(), backend_identity,
      arguments.context, arguments.batch_size,
      llmcc::EntropyReductionName(arguments.entropy_reduction),
      device_reduction ? "device" : "host");
  if (!text) {
    Emit(ConfigurationJson(arguments, requested_model, &identity));
  }
  for (const auto& message : discovery.warnings) {
    warning(message);
  }
  if (!arguments.no_cache &&
      std::ranges::any_of(discovery.sources, [](const auto& source) {
        return !source.repository.has_value();
      })) {
    warning("entropy caching is disabled for inputs outside Git worktrees");
  }
  if (!arguments.no_cache) {
    std::set<std::filesystem::path> repositories;
    for (const auto& source : discovery.sources) {
      if (source.repository.has_value()) {
        repositories.insert(*source.repository);
      }
    }
    for (const auto& repository : repositories) {
      try {
        static_cast<void>(llmcc::GetRepositoryCacheStatus(repository));
      } catch (const std::exception& error) {
        warning("entropy cache is unavailable for " + repository.string() +
                ": " + error.what());
      }
    }
  }

  llmcc::ProjectAnalyzer analyzer(
      {.model = identity,
       .tau_rule =
           arguments.tau_percentile.has_value()
               ? llmcc::TauRule{.kind = llmcc::TauRule::Kind::kPercentile,
                                .value = *arguments.tau_percentile}
               : llmcc::TauRule{.kind = llmcc::TauRule::Kind::kAbsolute,
                                .value = arguments.tau.value_or(0.67)},
       .alpha = arguments.alpha,
       .cache = !arguments.no_cache,
       .hotspots = arguments.hotspots,
       .hierarchy_mode = arguments.hierarchy_mode},
      [&]() {
        progress.Phase("loading model after entropy cache miss");
        return std::make_unique<LlamaEntropyProvider>(
            model_cache, identity.canonical_path, arguments.context,
            arguments.gpu_layers, resolved_backend, arguments.batch_size,
            arguments.entropy_reduction,
            [&](std::size_t completed, std::size_t total) {
              progress.Tokens(completed, total);
            });
      });

  MetricTotals totals;
  totals.discovered = discovery.sources.size();
  std::map<std::string, MetricTotals> languages;
  for (const auto& source : discovery.sources) {
    ++languages[std::string(llmcc::LanguageName(source.language))].discovered;
  }
  bool fatal = false;
  std::size_t file_index = 0;
  for (const auto& source : discovery.sources) {
    progress.StartFile(++file_index, discovery.sources.size(), source.path);
    const std::string language(llmcc::LanguageName(source.language));
    if (!text) {
      Emit({{"type", "file_start"},
            {"path", source.path.string()},
            {"language", language}});
    }
    try {
      const std::string contents = ReadFile(source.path);
      auto result = analyzer.AnalyzeFile(source, contents);
      progress.FinishFile(result.entropy_cache_hit);
      if (text) {
        PrintFileText(source, contents, result, arguments.score_mode);
      } else {
        nlohmann::json event = llmcc::AnalysisJson(result.analysis);
        event["type"] = "file";
        event["path"] = source.path.string();
        event["language"] = language;
        event["entropy_cache_hit"] = result.entropy_cache_hit;
        event["score"] =
            ScoreJson(result.analysis.metrics, arguments.score_mode);
        event["score_mode"] = arguments.score_mode;
        event["functions"] = nlohmann::json::array();
        for (const llmcc::FunctionScore& function : result.functions) {
          event["functions"].push_back(
              FunctionJson(function, arguments.score_mode));
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
        Emit(event);
      }
      ++totals.analyzed;
      Accumulate(result.analysis, totals);
      auto& language_totals = languages[language];
      ++language_totals.analyzed;
      Accumulate(result.analysis, language_totals);
    } catch (const llmcc::ScorerInitializationError& error) {
      progress.FailFile();
      ++totals.failed;
      ++languages[language].failed;
      if (text) {
        std::cerr << "error: " << TerminalSafe(source.path.string()) << ": "
                  << TerminalSafe(error.what()) << '\n';
      } else {
        Emit({{"type", "error"},
              {"path", source.path.string()},
              {"language", language},
              {"message", error.what()},
              {"fatal", true}});
      }
      fatal = true;
    } catch (const std::exception& error) {
      progress.FailFile();
      ++totals.failed;
      ++languages[language].failed;
      if (text) {
        std::cerr << "error: " << TerminalSafe(source.path.string()) << ": "
                  << TerminalSafe(error.what()) << '\n';
      } else {
        Emit({{"type", "error"},
              {"path", source.path.string()},
              {"language", language},
              {"message", error.what()}});
      }
    }
  }
  if (text) {
    PrintTotalsText(totals, arguments.score_mode);
  } else {
    Emit(TotalsJson(totals, languages, fatal, arguments.score_mode,
                    arguments.hierarchy_mode));
  }
  if (fatal) {
    return 2;
  }
  return totals.failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
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
    } else if (argc > 1 && std::string_view(argv[1]) == "cache") {
      result = RunCache(argc, argv);
    } else {
      result = RunAnalyze(ParseAnalyzeArguments(argc, argv));
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
