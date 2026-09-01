#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
  std::uint32_t context = llmcc::kDefaultContextSize;
  double tau_percentile = 67.0;
  double alpha = 0.8;
};

constexpr std::string_view kUsageBeforeContext =
    "Usage:\n"
    "  llm-cc PATH... [--lang auto|rust|c|cpp] [OPTIONS]\n"
    "  llm-cc score --model GGUF [--prompt TEXT | --file PATH] [OPTIONS]\n"
    "  llm-cc models list|remove FILE|path\n"
    "  llm-cc cache status|prune|clear [PATH] [--format text|json]\n\n"
    "Analysis options:\n"
    "  --lang auto|rust|c|cpp  infer language or force it (default: auto)\n"
    "  --include-headers     include headers during recursive discovery\n"
    "  --no-ignore           include ignored and generated source files\n"
    "  --no-cache            disable repository-local entropy caching\n"
    "  --no-download         do not fetch the default model\n"
    "  --model GGUF          llama.cpp-compatible model\n"
    "  --context N           maximum input tokens (default: ";

constexpr std::string_view kUsageAfterContext =
    ")\n"
    "  --tau-percentile N    entropy percentile (default: 67)\n"
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
  } else if (option == "--context") {
    arguments.context = ParseNumber<std::uint32_t>(option, value);
    if (arguments.context == 0) {
      Usage("--context must be positive");
    }
  } else if (option == "--tau-percentile") {
    arguments.tau_percentile = ParseNumber<double>(option, value);
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
  if (!std::isfinite(arguments.tau_percentile) ||
      arguments.tau_percentile < 0.0 || arguments.tau_percentile > 100.0) {
    Usage("--tau-percentile must be finite and between 0 and 100");
  }
  if (!std::isfinite(arguments.alpha) || arguments.alpha < 0.0 ||
      arguments.alpha > 1.0) {
    Usage("--alpha must be finite and between 0 and 1");
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
  }
  const auto status = llmcc::GetRepositoryCacheStatus(*repository);
  if (arguments.format == "json") {
    std::cout << nlohmann::json{{"repository", status.repository.string()},
                                {"directory", status.directory.string()},
                                {"entries", status.entries},
                                {"bytes", status.bytes}}
                     .dump()
              << '\n';
  } else {
    std::cout << "repository: " << status.repository.string() << '\n'
              << "directory: " << status.directory.string() << '\n'
              << "entries: " << status.entries << '\n'
              << "bytes: " << status.bytes << '\n';
  }
  return 0;
}

class LlamaEntropyProvider : public llmcc::EntropyProvider {
 public:
  LlamaEntropyProvider(const std::filesystem::path& cache_dir,
                       const std::filesystem::path& model,
                       std::uint32_t context)
      : scorer_((llmcc::MarkCachedModelUsed(cache_dir, model), model),
                {.context_size = context}) {}

  std::vector<llmcc::EntropyRecord> Score(std::string_view source) override {
    return llmcc::ParseEntropyJsonl(scorer_.Score(source));
  }

 private:
  llmcc::EntropyScorer scorer_;
};

struct MetricTotals {
  std::uint64_t discovered = 0;
  std::uint64_t analyzed = 0;
  std::uint64_t failed = 0;
  double llm_cc = 0.0;
  std::uint64_t total_branch = 0;
  std::uint64_t total_comp_level = 0;
};

nlohmann::json TotalsJson(const MetricTotals& totals,
                          const std::map<std::string, MetricTotals>& languages,
                          bool fatal) {
  nlohmann::json language_json = nlohmann::json::object();
  for (const auto& [name, value] : languages) {
    language_json[name] = {{"discovered", value.discovered},
                           {"analyzed", value.analyzed},
                           {"failed", value.failed},
                           {"llm_cc", value.llm_cc},
                           {"total_branch", value.total_branch},
                           {"total_comp_level", value.total_comp_level}};
  }
  return {{"type", "totals"},
          {"discovered", totals.discovered},
          {"analyzed", totals.analyzed},
          {"failed", totals.failed},
          {"llm_cc", totals.llm_cc},
          {"total_branch", totals.total_branch},
          {"total_comp_level", totals.total_comp_level},
          {"languages", std::move(language_json)},
          {"partial", fatal || totals.failed != 0 ||
                          totals.analyzed != totals.discovered}};
}

nlohmann::json ConfigurationJson(
    const AnalyzeArguments& arguments, std::string_view requested_model,
    const llmcc::ModelIdentity* identity = nullptr) {
  nlohmann::json configuration = {
      {"type", "configuration"},
      {"language", arguments.language_name},
      {"include_headers", arguments.include_headers},
      {"no_ignore", arguments.no_ignore},
      {"no_download", arguments.no_download},
      {"model", requested_model},
      {"context", arguments.context},
      {"tau_percentile", arguments.tau_percentile},
      {"alpha", arguments.alpha},
      {"backend", llmcc::CompiledBackend()},
      {"inference_abi", llmcc::InferenceAbi()},
      {"cache",
       {{"enabled", !arguments.no_cache},
        {"version", 1},
        {"namespace", ".llm-cc-cache/llm-cc/v1/entropy"},
        {"limit_bytes", llmcc::kEntropyCacheLimit}}}};
  if (identity != nullptr) {
    configuration["model"] = identity->canonical_path.string();
    configuration["model_size"] = identity->size;
    configuration["model_modification_time"] = identity->modification_time;
  }
  return configuration;
}

int RunAnalyze(const AnalyzeArguments& arguments) {
  llmcc::DiscoveryResult discovery = llmcc::DiscoverSources(
      arguments.sources, {.language = arguments.language,
                          .include_headers = arguments.include_headers,
                          .no_ignore = arguments.no_ignore});
  const std::string requested_model =
      arguments.model.has_value() ? arguments.model->string() : "default";
  Emit({{"type", "start"},
        {"discovered", discovery.sources.size()},
        {"model", requested_model}});

  if (discovery.sources.empty()) {
    Emit(ConfigurationJson(arguments, requested_model));
    for (const auto& warning : discovery.warnings) {
      Emit({{"type", "warning"}, {"message", warning}});
    }
    Emit({{"type", "warning"},
          {"message", "no eligible source files were discovered"}});
    Emit(TotalsJson({}, {}, false));
    return 0;
  }

  const std::filesystem::path model_cache = llmcc::CacheDir();
  const auto resolved_model = llmcc::ResolveModel(
      arguments.model, arguments.no_download, std::filesystem::current_path(),
      model_cache, llmcc::DownloadDefaultModel);
  const auto identity =
      llmcc::InspectModel(resolved_model, llmcc::InferenceAbi(),
                          llmcc::CompiledBackend(), arguments.context);
  Emit(ConfigurationJson(arguments, requested_model, &identity));
  for (const auto& warning : discovery.warnings) {
    Emit({{"type", "warning"}, {"message", warning}});
  }
  if (!arguments.no_cache &&
      std::ranges::any_of(discovery.sources, [](const auto& source) {
        return !source.repository.has_value();
      })) {
    Emit({{"type", "warning"},
          {"message",
           "entropy caching is disabled for inputs outside Git worktrees"}});
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
        Emit({{"type", "warning"},
              {"message", "entropy cache is unavailable for " +
                              repository.string() + ": " + error.what()}});
      }
    }
  }

  llmcc::ProjectAnalyzer analyzer(
      {.model = identity,
       .tau_percentile = arguments.tau_percentile,
       .alpha = arguments.alpha,
       .cache = !arguments.no_cache},
      [&]() {
        return std::make_unique<LlamaEntropyProvider>(
            model_cache, identity.canonical_path, arguments.context);
      });

  MetricTotals totals;
  totals.discovered = discovery.sources.size();
  std::map<std::string, MetricTotals> languages;
  for (const auto& source : discovery.sources) {
    ++languages[std::string(llmcc::LanguageName(source.language))].discovered;
  }
  bool fatal = false;
  for (const auto& source : discovery.sources) {
    const std::string language(llmcc::LanguageName(source.language));
    Emit({{"type", "file_start"},
          {"path", source.path.string()},
          {"language", language}});
    try {
      auto result = analyzer.AnalyzeFile(source, ReadFile(source.path));
      nlohmann::json event = llmcc::AnalysisJson(result.analysis);
      event["type"] = "file";
      event["path"] = source.path.string();
      event["language"] = language;
      event["entropy_cache_hit"] = result.entropy_cache_hit;
      Emit(event);
      ++totals.analyzed;
      totals.llm_cc += result.analysis.llm_cc;
      totals.total_branch += result.analysis.total_branch;
      totals.total_comp_level += result.analysis.total_comp_level;
      auto& language_totals = languages[language];
      ++language_totals.analyzed;
      language_totals.llm_cc += result.analysis.llm_cc;
      language_totals.total_branch += result.analysis.total_branch;
      language_totals.total_comp_level += result.analysis.total_comp_level;
    } catch (const llmcc::ScorerInitializationError& error) {
      ++totals.failed;
      ++languages[language].failed;
      Emit({{"type", "error"},
            {"path", source.path.string()},
            {"language", language},
            {"message", error.what()},
            {"fatal", true}});
      fatal = true;
    } catch (const std::exception& error) {
      ++totals.failed;
      ++languages[language].failed;
      Emit({{"type", "error"},
            {"path", source.path.string()},
            {"language", language},
            {"message", error.what()}});
    }
  }
  Emit(TotalsJson(totals, languages, fatal));
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
