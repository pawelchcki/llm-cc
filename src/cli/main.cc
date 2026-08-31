#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "generated/version.h"
#include "src/cache.h"
#include "src/core.h"
#include "src/download.h"
#include "src/jsonl.h"
#include "src/lang.h"
#include "src/score_cmd.h"

namespace {

struct AnalyzeArguments {
  std::filesystem::path source;
  std::optional<std::string> language;
  std::optional<std::filesystem::path> entropy_jsonl;
  std::optional<std::filesystem::path> model;
  bool no_download = false;
  std::int32_t gpu_layers = 0;
  std::uint32_t context = llmcc::kDefaultContextSize;
  bool gpu_layers_set = false;
  bool context_set = false;
  double tau_percentile = 67.0;
  double alpha = 0.8;
};

constexpr std::string_view kUsageBeforeContext =
    "Usage:\n"
    "  llm-cc SOURCE [--lang rust|c|cpp] [--entropy-jsonl PATH | "
    "--model GGUF] [OPTIONS]\n"
    "  llm-cc score --model GGUF [--prompt TEXT | --file PATH] [OPTIONS]\n"
    "  llm-cc models list|remove FILE|path\n\n"
    "Analysis options:\n"
    "  --no-download          do not fetch the default model\n"
    "  --gpu-layers N         transformer layers to offload (-1 means all)\n"
    "  --context N            maximum input tokens (default: ";

constexpr std::string_view kUsageAfterContext =
    ")\n"
    "  --tau-percentile N     entropy percentile (default: 67)\n"
    "  --alpha N              branching weight (default: 0.8)\n"
    "  -V, --version          show the program version\n"
    "  -h, --help             show this help\n";

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
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size()) {
    Usage(std::string(option) + " expects a number, got '" +
          std::string(value) + "'");
  }
  return result;
}

void SetAnalyzeOption(AnalyzeArguments& arguments, std::string_view option,
                      std::string_view value) {
  if (option == "--lang") {
    arguments.language = value;
  } else if (option == "--entropy-jsonl") {
    arguments.entropy_jsonl = value;
  } else if (option == "--model") {
    arguments.model = value;
  } else if (option == "--gpu-layers") {
    arguments.gpu_layers = ParseNumber<std::int32_t>(option, value);
    arguments.gpu_layers_set = true;
    if (arguments.gpu_layers < -1) {
      Usage("--gpu-layers must be -1 or greater");
    }
  } else if (option == "--context") {
    arguments.context = ParseNumber<std::uint32_t>(option, value);
    arguments.context_set = true;
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
    if (!option.starts_with('-')) {
      if (!arguments.source.empty()) {
        Usage("only one source file may be analyzed");
      }
      arguments.source = option;
      continue;
    }
    if (index + 1 >= argc) {
      Usage(std::string(option) + " requires a value");
    }
    SetAnalyzeOption(arguments, option, argv[++index]);
  }
  if (arguments.source.empty()) {
    Usage("a source file is required unless a subcommand is used");
  }
  if (arguments.entropy_jsonl.has_value() &&
      (arguments.gpu_layers_set || arguments.context_set)) {
    Usage("--gpu-layers and --context cannot be used with --entropy-jsonl");
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

std::string LoadEntropyJsonl(const AnalyzeArguments& arguments,
                             const std::filesystem::path& cache_dir,
                             const std::optional<std::filesystem::path>& model,
                             std::string_view preprocessed) {
  if (arguments.entropy_jsonl.has_value()) {
    return ReadFile(*arguments.entropy_jsonl);
  }
  if (!model.has_value()) {
    throw std::logic_error("model resolution returned no model");
  }
  llmcc::MarkCachedModelUsed(cache_dir, *model);
  return llmcc::ScoreEntropyJsonl(
      *model, preprocessed,
      {.gpu_layers = arguments.gpu_layers, .context_size = arguments.context});
}

int RunAnalyze(const AnalyzeArguments& arguments) {
  const std::filesystem::path cache_dir = llmcc::CacheDir();
  const auto model = llmcc::ResolveModel(
      arguments.model, arguments.entropy_jsonl, arguments.no_download,
      std::filesystem::current_path(), cache_dir, llmcc::DownloadDefaultModel);
  const llmcc::Language language =
      arguments.language.has_value()
          ? llmcc::ParseLanguage(*arguments.language)
          : llmcc::InferLanguage(arguments.source.string());
  const std::string source = ReadFile(arguments.source);
  auto [preprocessed, offset_map] = llmcc::StripComments(source, language);
  const auto events = llmcc::StructuralEvents(preprocessed, language);

  const std::string jsonl =
      LoadEntropyJsonl(arguments, cache_dir, model, preprocessed);
  const auto records = llmcc::ParseEntropyJsonl(jsonl);
  const auto tokens = llmcc::AlignTokens(preprocessed, records);
  llmcc::Analysis analysis =
      llmcc::Analyze(tokens, events, arguments.tau_percentile, arguments.alpha);
  llmcc::MapAnalysisOffsets(analysis, offset_map);
  std::cout << llmcc::PrettyAnalysisJson(analysis);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    int result = 0;
    if (argc > 1 && std::string_view(argv[1]) == "score") {
      result = llmcc::RunScoreCommand(argc - 1, argv + 1);
    } else if (argc > 1 && std::string_view(argv[1]) == "models") {
      result = RunModels(argc, argv);
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
    return 1;
  }
}
