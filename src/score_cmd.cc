#include "src/score_cmd.h"

#include <ggml-backend.h>
#include <llama.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

#include "generated/version.h"
#include "src/backend.h"
#include "src/cache.h"
#include "src/download.h"
#include "src/inference_guard.h"
#include "src/models.h"
#include "src/scoring.h"

namespace {

enum class BosMode : std::uint8_t { kAuto, kAlways, kNever };

constexpr std::size_t kDecodeBatchSize = 64;

struct Arguments {
  std::filesystem::path model;
  std::optional<std::string> model_name;
  std::optional<std::string> prompt;
  std::optional<std::filesystem::path> file;
  BosMode bos = BosMode::kAuto;
  std::uint32_t context_size = llmcc::kDefaultContextSize;
  std::int32_t threads = 0;
  std::int32_t gpu_layers = 0;
  llmcc::BackendKind backend = llmcc::BackendKind::kAuto;
  std::optional<std::filesystem::path> backend_directory;
  bool entropy = false;
  bool no_download = false;
  bool override_memory_check = false;
};

constexpr std::string_view kUsageBeforeContext =
    "Usage: llm-cc score (--model MODEL.gguf | --model-name NAME) "
    "[INPUT] [OPTIONS]\n\n"
    "Teacher-force input through a GGUF model and emit observed-token "
    "probabilities.\n"
    "No continuation is generated. Output is JSONL on stdout.\n\n"
    "Input (choose at most one; otherwise stdin):\n"
    "  --prompt TEXT          score literal text\n"
    "  --file PATH            score the contents of a file\n\n"
    "Options:\n"
    "  --model PATH           local llama.cpp-compatible GGUF\n"
    "  --model-name NAME      registered model name\n"
    "  --no-download          do not fetch the model or backend bundle\n"
    "  --bos auto|always|never  beginning-of-stream policy (default: auto)\n"
    "  --context-size N       maximum tokens in the input (default: ";

constexpr std::string_view kUsageAfterContext =
    ")\n"
    "  --threads N            inference threads (default: hardware count)\n"
    "  --gpu-layers N         layers to offload; -1 means all (default: 0)\n"
    "  --backend NAME         auto, cpu, cuda, or rocm (default: auto)\n"
    "  --backend-dir DIR      GPU backend bundle/shared-library directory\n"
    "  --override-memory-check  bypass the preflight memory check\n"
    "  --entropy              emit full-vocabulary next-token entropy\n"
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

template <typename Integer>
Integer ParseInteger(std::string_view name, std::string_view value) {
  Integer parsed{};
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    Usage(std::string(name) + " expects an integer, got '" +
          std::string(value) + "'");
  }
  return parsed;
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

bool ShouldFetchBackend(llmcc::BackendKind backend, std::int32_t gpu_layers) {
  return backend == llmcc::BackendKind::kCuda ||
         backend == llmcc::BackendKind::kRocm ||
         (backend == llmcc::BackendKind::kAuto && gpu_layers != 0);
}

void ApplyBackendDirectoryEnvironment(Arguments& arguments) {
  if (arguments.backend_directory.has_value() ||
      !ShouldFetchBackend(arguments.backend, arguments.gpu_layers)) {
    return;
  }
  if (const char* environment = std::getenv("LLM_CC_BACKEND_DIR");
      environment != nullptr && *environment != '\0') {
    arguments.backend_directory = environment;
  }
}

void SetOption(Arguments& arguments, std::string_view option,
               std::string_view value) {
  if ((option == "--prompt" && arguments.file.has_value()) ||
      (option == "--file" && arguments.prompt.has_value())) {
    Usage("--prompt and --file are mutually exclusive");
  }
  if (option == "--model") {
    arguments.model = value;
  } else if (option == "--model-name") {
    arguments.model_name = value;
  } else if (option == "--prompt") {
    arguments.prompt = value;
  } else if (option == "--file") {
    arguments.file = value;
  } else if (option == "--bos") {
    if (value == "auto") {
      arguments.bos = BosMode::kAuto;
    } else if (value == "always") {
      arguments.bos = BosMode::kAlways;
    } else if (value == "never") {
      arguments.bos = BosMode::kNever;
    } else {
      Usage("--bos expects auto, always, or never");
    }
  } else if (option == "--context-size") {
    arguments.context_size = ParseInteger<std::uint32_t>(option, value);
    if (arguments.context_size == 0) {
      Usage("--context-size must be positive");
    }
  } else if (option == "--threads") {
    arguments.threads = ParseInteger<std::int32_t>(option, value);
    if (arguments.threads < 1) {
      Usage("--threads must be positive");
    }
  } else if (option == "--gpu-layers") {
    arguments.gpu_layers = ParseInteger<std::int32_t>(option, value);
    if (arguments.gpu_layers < -1) {
      Usage("--gpu-layers must be -1 or greater");
    }
  } else if (option == "--backend") {
    try {
      arguments.backend = llmcc::ParseBackend(value);
    } catch (const std::invalid_argument& error) {
      Usage(error.what());
    }
  } else if (option == "--backend-dir") {
    arguments.backend_directory = value;
  } else {
    Usage("unknown option: " + std::string(option));
  }
}

Arguments ParseArguments(int argc, char** argv) {
  Arguments arguments;
  for (int i = 1; i < argc; ++i) {
    const std::string_view option = argv[i];
    if (option == "-h" || option == "--help") {
      Usage();
    }
    if (option == "-V" || option == "--version") {
      std::cout << "llm-cc " << LLM_CC_VERSION << '\n';
      std::exit(0);
    }
    if (option == "--entropy") {
      arguments.entropy = true;
      continue;
    }
    if (option == "--no-download") {
      arguments.no_download = true;
      continue;
    }
    if (option == "--override-memory-check") {
      arguments.override_memory_check = true;
      continue;
    }
    if (i + 1 >= argc) {
      Usage(std::string(option) + " requires a value");
    }
    SetOption(arguments, option, argv[++i]);
  }
  if (!arguments.model.empty() && arguments.model_name.has_value()) {
    Usage("--model-name and --model are mutually exclusive");
  }
  if (arguments.model_name.has_value() &&
      llmcc::FindModel(*arguments.model_name) == nullptr) {
    Usage("unknown model name '" + *arguments.model_name + "'; " +
          ValidModelNames());
  }
  if (arguments.model.empty() && !arguments.model_name.has_value()) {
    Usage("--model or --model-name is required");
  }
  ApplyBackendDirectoryEnvironment(arguments);
  if (arguments.backend_directory.has_value()) {
    std::error_code error;
    if (!std::filesystem::is_directory(*arguments.backend_directory, error)) {
      Usage("--backend-dir is not a directory: " +
            arguments.backend_directory->string());
    }
  }
  if (arguments.threads == 0) {
    arguments.threads = static_cast<std::int32_t>(
        std::max(1U, std::thread::hardware_concurrency()));
  }
  try {
    static_cast<void>(
        llmcc::SelectBackend(arguments.backend, arguments.gpu_layers, {}));
  } catch (const std::invalid_argument& error) {
    Usage(error.what());
  } catch (const std::runtime_error&) {  // NOLINT(bugprone-empty-catch)
    // Device availability is checked after the selected plugins are loaded.
  }
  return arguments;
}

std::string ReadStream(std::istream& stream) {
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

std::string ReadInput(const Arguments& arguments) {
  if (arguments.prompt.has_value()) {
    return *arguments.prompt;
  }
  if (arguments.file.has_value()) {
    std::ifstream input(*arguments.file, std::ios::binary);
    if (!input) {
      throw std::runtime_error("cannot open input file: " +
                               arguments.file->string());
    }
    return ReadStream(input);
  }
  return ReadStream(std::cin);
}

std::vector<llama_token> Tokenize(const llama_vocab* vocab,
                                  const std::string& text) {
  if (text.size() >
      static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    throw std::runtime_error("input is too large for the tokenizer");
  }
  const auto text_size = static_cast<std::int32_t>(text.size());
  std::int32_t count =
      llama_tokenize(vocab, text.data(), text_size, nullptr, 0, false, true);
  if (count == std::numeric_limits<std::int32_t>::min()) {
    throw std::runtime_error("token count overflow");
  }
  if (count > 0) {
    throw std::runtime_error("tokenizer returned an unexpected size probe");
  }
  std::vector<llama_token> tokens(static_cast<std::size_t>(-count));
  count = llama_tokenize(vocab, text.data(), text_size, tokens.data(),
                         static_cast<std::int32_t>(tokens.size()), false, true);
  if (count < 0) {
    throw std::runtime_error("tokenization failed");
  }
  tokens.resize(static_cast<std::size_t>(count));
  return tokens;
}

std::string TokenPiece(const llama_vocab* vocab, llama_token token) {
  std::string piece(32, '\0');
  std::int32_t size =
      llama_token_to_piece(vocab, token, piece.data(),
                           static_cast<std::int32_t>(piece.size()), 0, true);
  if (size < 0) {
    piece.resize(static_cast<std::size_t>(-size));
    size =
        llama_token_to_piece(vocab, token, piece.data(),
                             static_cast<std::int32_t>(piece.size()), 0, true);
  }
  if (size < 0) {
    throw std::runtime_error("could not decode token piece");
  }
  piece.resize(static_cast<std::size_t>(size));
  return piece;
}

void WriteNullScore(std::ostream& output, std::size_t position,
                    llama_token token, std::string_view piece,
                    bool emit_entropy) {
  output << "{\"position\":" << position << ",\"token_id\":" << token
         << ",\"piece\":\"" << llmcc::JsonEscapeBytes(piece)
         << "\",\"bytes_hex\":\"" << llmcc::BytesToHex(piece)
         << "\",\"probability\":null,\"log_probability\":null";
  if (emit_entropy) {
    output << ",\"entropy\":null";
  }
  output << "}\n";
}

void WriteScore(std::ostream& output, std::size_t position, llama_token token,
                std::string_view piece, const llmcc::TokenScore& score,
                bool emit_entropy) {
  output << std::setprecision(17) << "{\"position\":" << position
         << ",\"token_id\":" << token << ",\"piece\":\""
         << llmcc::JsonEscapeBytes(piece) << "\",\"bytes_hex\":\""
         << llmcc::BytesToHex(piece)
         << "\",\"probability\":" << score.probability
         << ",\"log_probability\":" << score.log_probability;
  if (emit_entropy) {
    if (!score.entropy.has_value()) {
      throw std::logic_error("entropy was requested but not computed");
    }
    output << ",\"entropy\":" << *score.entropy;
  }
  output << "}\n";
}

std::uint64_t ModelFileSize(const std::filesystem::path& path) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    throw std::runtime_error("could not stat model: " + path.string() + ": " +
                             error.message());
  }
  if (size > std::numeric_limits<std::uint64_t>::max()) {
    throw std::runtime_error("model file is too large to measure: " +
                             path.string());
  }
  return static_cast<std::uint64_t>(size);
}

std::optional<std::uint64_t> HostAvailableMemory() {
#ifdef __APPLE__
  vm_statistics64_data_t statistics{};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                        reinterpret_cast<host_info64_t>(&statistics),
                        &count) != KERN_SUCCESS) {
    return std::nullopt;
  }
  vm_size_t page_size = 0;
  if (host_page_size(mach_host_self(), &page_size) != KERN_SUCCESS ||
      page_size == 0) {
    return std::nullopt;
  }
  const std::uint64_t available_pages =
      static_cast<std::uint64_t>(statistics.free_count) +
      static_cast<std::uint64_t>(statistics.inactive_count) +
      static_cast<std::uint64_t>(statistics.purgeable_count);
  if (available_pages > std::numeric_limits<std::uint64_t>::max() / page_size) {
    return std::nullopt;
  }
  return available_pages * page_size;
#else
  std::ifstream input("/proc/meminfo");
  std::string field;
  std::uint64_t kibibytes = 0;
  std::string unit;
  while (input >> field >> kibibytes >> unit) {
    if (field != "MemAvailable:") {
      continue;
    }
    if (unit != "kB" ||
        kibibytes > std::numeric_limits<std::uint64_t>::max() / 1024) {
      return std::nullopt;
    }
    return kibibytes * 1024;
  }
  return std::nullopt;
#endif
}

std::optional<std::uint64_t> GpuAvailableMemory() {
  std::uint64_t aggregate = 0;
  bool found = false;
  for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
    ggml_backend_dev_t device = ggml_backend_dev_get(i);
    const enum ggml_backend_dev_type type = ggml_backend_dev_type(device);
    if (type != GGML_BACKEND_DEVICE_TYPE_GPU &&
        type != GGML_BACKEND_DEVICE_TYPE_IGPU) {
      continue;
    }
    std::size_t free_bytes = 0;
    std::size_t total_bytes = 0;
    ggml_backend_dev_memory(device, &free_bytes, &total_bytes);
    found = true;
    const auto memory = static_cast<std::uint64_t>(free_bytes);
    aggregate = memory > std::numeric_limits<std::uint64_t>::max() - aggregate
                    ? std::numeric_limits<std::uint64_t>::max()
                    : aggregate + memory;
  }
  return found ? std::optional<std::uint64_t>(aggregate) : std::nullopt;
}

void CheckAvailableMemory(const Arguments& arguments, bool use_gpu,
                          const std::optional<std::uint64_t>& gpu_available) {
  if (const auto warning = llmcc::CodexSandboxGpuWarning(
          gpu_available, std::getenv("CODEX_SANDBOX"));
      warning.has_value()) {
    std::cerr << "warning: " << *warning << '\n';
  }
  if (arguments.override_memory_check) {
    return;
  }
  if (arguments.gpu_layers > 0) {
    std::cerr << "warning: partial GPU offload memory use depends on model "
                 "architecture; skipping memory check\n";
    return;
  }

  const std::uint64_t model_bytes = ModelFileSize(arguments.model);
  const std::optional<std::uint64_t> host_available = HostAvailableMemory();
  if (!host_available.has_value()) {
    if (use_gpu && !gpu_available.has_value()) {
      const llmcc::MemoryCheckResult result =
          llmcc::CheckMemory(model_bytes, 0, gpu_available, true, false);
      throw std::runtime_error(result.error);
    }
    std::cerr << "warning: could not determine available host memory; "
                 "skipping memory check\n";
    return;
  }

  const llmcc::MemoryCheckResult result = llmcc::CheckMemory(
      model_bytes, *host_available, gpu_available, use_gpu, false);
  if (!result.ok) {
    throw std::runtime_error(result.error);
  }
}

using Model = std::unique_ptr<llama_model, decltype(&llama_model_free)>;
using Context = std::unique_ptr<llama_context, decltype(&llama_free)>;

struct ScoreOptions {
  BosMode bos;
  std::uint32_t context_size;
  std::int32_t threads;
  bool entropy;
  std::string_view context_option;
};

void WriteSummary(std::ostream* diagnostics, std::size_t tokens,
                  std::size_t scored, double negative_log_likelihood) {
  if (diagnostics == nullptr) {
    return;
  }
  *diagnostics << "tokens=" << tokens << " scored=" << scored;
  if (scored == 0) {
    *diagnostics << " mean_nll=null perplexity=null\n";
    return;
  }
  const double mean_nll = negative_log_likelihood / static_cast<double>(scored);
  *diagnostics << std::setprecision(10) << " mean_nll=" << mean_nll
               << " perplexity=" << std::exp(mean_nll) << '\n';
}

void ScoreInput(llama_model* model, llmcc::BackendLogCapture& backend_log,
                std::string_view input, const ScoreOptions& options,
                std::ostream& output, std::ostream* diagnostics) {
  const llama_vocab* vocabulary = llama_model_get_vocab(model);
  const bool prepend_bos =
      options.bos == BosMode::kAlways ||
      (options.bos == BosMode::kAuto && llama_vocab_get_add_bos(vocabulary));
  std::vector<llama_token> tokens = Tokenize(vocabulary, std::string(input));
  if (prepend_bos) {
    const llama_token bos_token = llama_vocab_bos(vocabulary);
    if (bos_token < 0) {
      throw std::runtime_error("the model vocabulary has no BOS token");
    }
    tokens.insert(tokens.begin(), bos_token);
  }
  const std::size_t first_observed = prepend_bos ? 1 : 0;
  if (tokens.size() > options.context_size) {
    throw std::runtime_error("input token count " +
                             std::to_string(tokens.size()) + " exceeds " +
                             std::string(options.context_option) + " " +
                             std::to_string(options.context_size));
  }
  if (tokens.empty()) {
    WriteSummary(diagnostics, 0, 0, 0.0);
    return;
  }
  if (!prepend_bos) {
    WriteNullScore(output, 0, tokens.front(),
                   TokenPiece(vocabulary, tokens.front()), options.entropy);
  }
  if (tokens.size() == 1) {
    WriteSummary(diagnostics, tokens.size() - first_observed, 0, 0.0);
    return;
  }

  const std::size_t batch_size = std::min(kDecodeBatchSize, tokens.size() - 1);
  llama_context_params parameters = llama_context_default_params();
  parameters.n_ctx = static_cast<std::uint32_t>(tokens.size());
  parameters.n_batch = static_cast<std::uint32_t>(batch_size);
  parameters.n_ubatch = static_cast<std::uint32_t>(batch_size);
  parameters.n_seq_max = 1;
  parameters.n_outputs_max = static_cast<std::uint32_t>(batch_size);
  parameters.n_outputs_max_per_seq = static_cast<std::uint32_t>(batch_size);
  parameters.n_threads = options.threads;
  parameters.n_threads_batch = options.threads;
  Context context(llama_init_from_model(model, parameters), llama_free);
  if (!context) {
    const std::string detail = backend_log.Error();
    throw std::runtime_error("could not create inference context" +
                             (detail.empty() ? std::string() : ": " + detail));
  }

  const std::int32_t vocabulary_size = llama_vocab_n_tokens(vocabulary);
  double negative_log_likelihood = 0.0;
  std::size_t scored = 0;
  std::vector<llama_pos> positions(batch_size);
  std::vector<std::int32_t> sequence_counts(batch_size, 1);
  llama_seq_id sequence = 0;
  std::vector<llama_seq_id*> sequences(batch_size, &sequence);
  std::vector<std::int8_t> output_logits(batch_size, 1);
  for (std::size_t source = 0; source + 1 < tokens.size();
       source += batch_size) {
    const std::size_t count = std::min(batch_size, tokens.size() - 1 - source);
    for (std::size_t index = 0; index < count; ++index) {
      positions[index] = static_cast<llama_pos>(source + index);
    }
    llama_batch batch = {
        .n_tokens = static_cast<std::int32_t>(count),
        .token = tokens.data() + source,
        .embd = nullptr,
        .pos = positions.data(),
        .n_seq_id = sequence_counts.data(),
        .seq_id = sequences.data(),
        .logits = output_logits.data(),
    };
    const int decode_result = llama_decode(context.get(), batch);
    if (decode_result != 0) {
      throw std::runtime_error("llama_decode failed at token " +
                               std::to_string(source) + " with code " +
                               std::to_string(decode_result));
    }
    for (std::size_t index = 0; index < count; ++index) {
      float* logits =
          llama_get_logits_ith(context.get(), static_cast<std::int32_t>(index));
      if (logits == nullptr) {
        throw std::runtime_error("model returned no logits");
      }
      const std::size_t target_index = source + index + 1;
      const llama_token target = tokens[target_index];
      if (target < 0 || target >= vocabulary_size) {
        throw std::runtime_error(
            "tokenizer produced a token outside the vocabulary");
      }
      const llmcc::TokenScore score = llmcc::ScoreToken(
          std::span<const float>(logits,
                                 static_cast<std::size_t>(vocabulary_size)),
          static_cast<std::size_t>(target), options.entropy);
      WriteScore(output, target_index - first_observed, target,
                 TokenPiece(vocabulary, target), score, options.entropy);
      negative_log_likelihood -= score.log_probability;
      ++scored;
    }
  }
  WriteSummary(diagnostics, tokens.size() - first_observed, scored,
               negative_log_likelihood);
}

int Run(const Arguments& arguments, std::ostream& output,
        std::ostream& diagnostics,
        const std::optional<std::string>& input_override = std::nullopt) {
  const std::string input =
      input_override.has_value() ? *input_override : ReadInput(arguments);
  llmcc::BackendLogCapture backend_log;
  llmcc::BackendRuntime backend(
      arguments.backend, arguments.gpu_layers, LLM_CC_VERSION,
      arguments.backend_directory, arguments.no_download,
      ShouldFetchBackend(arguments.backend, arguments.gpu_layers));
  llmcc::InferenceGuard inference_guard(llmcc::BackendName(backend.selected()));
  const bool use_gpu = arguments.gpu_layers != 0;
  const std::optional<std::uint64_t> gpu_available =
      use_gpu ? GpuAvailableMemory() : std::nullopt;
  CheckAvailableMemory(arguments, use_gpu, gpu_available);

  llama_model_params model_parameters = llama_model_default_params();
  model_parameters.n_gpu_layers = arguments.gpu_layers;
  Model model(
      llama_model_load_from_file(arguments.model.c_str(), model_parameters),
      llama_model_free);
  if (!model) {
    const std::string detail = backend_log.Error();
    throw std::runtime_error(
        "could not load model: " + arguments.model.string() +
        (detail.empty() ? std::string() : ": " + detail));
  }
  ScoreInput(model.get(), backend_log, input,
             {.bos = arguments.bos,
              .context_size = arguments.context_size,
              .threads = arguments.threads,
              .entropy = arguments.entropy,
              .context_option = "--context-size"},
             output, &diagnostics);
  return 0;
}

}  // namespace

namespace llmcc {

class EntropyScorer::Impl {
 public:
  Impl(const std::filesystem::path& model_path,
       const InferenceOptions& inference_options)
      : backend_(inference_options.backend, inference_options.gpu_layers,
                 LLM_CC_VERSION, inference_options.backend_directory,
                 inference_options.no_download,
                 inference_options.fetch_backend &&
                     ShouldFetchBackend(inference_options.backend,
                                        inference_options.gpu_layers)),
        inference_guard_(BackendName(backend_.selected())),
        model_(nullptr, llama_model_free),
        context_limit_(inference_options.context_size),
        threads_(static_cast<std::int32_t>(
            std::max(1U, std::thread::hardware_concurrency()))) {
    Arguments arguments;
    arguments.model = model_path;
    arguments.context_size = context_limit_;
    arguments.threads = threads_;
    arguments.gpu_layers = inference_options.gpu_layers;
    arguments.backend = inference_options.backend;
    arguments.backend_directory = inference_options.backend_directory;
    arguments.entropy = true;
    const bool use_gpu = inference_options.gpu_layers != 0;
    const auto gpu_available = use_gpu ? GpuAvailableMemory() : std::nullopt;
    CheckAvailableMemory(arguments, use_gpu, gpu_available);
    llama_model_params parameters = llama_model_default_params();
    parameters.n_gpu_layers = inference_options.gpu_layers;
    model_.reset(llama_model_load_from_file(model_path.c_str(), parameters));
    if (!model_) {
      const std::string detail = backend_log_.Error();
      throw std::runtime_error(
          "could not load model: " + model_path.string() +
          (detail.empty() ? std::string() : ": " + detail));
    }
  }

  std::string Score(std::string_view input) {
    std::ostringstream output;
    ScoreInput(model_.get(), backend_log_, input,
               {.bos = BosMode::kAuto,
                .context_size = context_limit_,
                .threads = threads_,
                .entropy = true,
                .context_option = "--context"},
               output, nullptr);
    return output.str();
  }

 private:
  BackendLogCapture backend_log_;
  BackendRuntime backend_;
  InferenceGuard inference_guard_;
  Model model_;
  std::uint32_t context_limit_;
  std::int32_t threads_;
};

EntropyScorer::EntropyScorer(const std::filesystem::path& model,
                             const InferenceOptions& options)
    : implementation_(std::make_unique<Impl>(model, options)) {}

EntropyScorer::~EntropyScorer() = default;
EntropyScorer::EntropyScorer(EntropyScorer&&) noexcept = default;
EntropyScorer& EntropyScorer::operator=(EntropyScorer&&) noexcept = default;

std::string EntropyScorer::Score(std::string_view input) {
  return implementation_->Score(input);
}

std::string_view InferenceAbi() {
  return "llama.cpp-c589f0ed10c643678c4707dd160c21ac7633ebc0/entropy-v1";
}

std::string_view CompiledBackend() {
#ifdef LLMCC_BACKEND_UNIVERSAL
  return "universal";
#elif defined LLMCC_BACKEND_CUDA
  return "cuda";
#elif defined LLMCC_BACKEND_ROCM
  return "rocm";
#elif defined LLMCC_BACKEND_METAL
  return "metal";
#else
  return "cpu";
#endif
}

std::string ScoreEntropyJsonl(const std::filesystem::path& model,
                              std::string_view input,
                              const InferenceOptions& options) {
  return EntropyScorer(model, options).Score(input);
}

int RunScoreCommand(int argc, char** argv) {
  try {
    Arguments arguments = ParseArguments(argc, argv);
    if (arguments.model_name.has_value()) {
      arguments.model =
          ResolveModel(std::nullopt, *FindModel(*arguments.model_name),
                       arguments.no_download, std::filesystem::current_path(),
                       CacheDir(), DownloadModel);
    }
    return Run(arguments, std::cout, std::cerr);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}

}  // namespace llmcc
