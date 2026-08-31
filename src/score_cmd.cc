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
#include <thread>
#include <vector>

#include "generated/version.h"
#include "src/scoring.h"

namespace {

enum class BosMode : std::uint8_t { kAuto, kAlways, kNever };

struct Arguments {
  std::filesystem::path model;
  std::optional<std::string> prompt;
  std::optional<std::filesystem::path> file;
  BosMode bos = BosMode::kAuto;
  std::uint32_t context_size = llmcc::kDefaultContextSize;
  std::int32_t threads = 0;
  std::int32_t gpu_layers = 0;
  bool entropy = false;
  bool override_memory_check = false;
};

constexpr std::string_view kUsageBeforeContext =
    "Usage: llm-cc --model MODEL.gguf [INPUT] [OPTIONS]\n\n"
    "Teacher-force input through a GGUF model and emit observed-token "
    "probabilities.\n"
    "No continuation is generated. Output is JSONL on stdout.\n\n"
    "Input (choose at most one; otherwise stdin):\n"
    "  --prompt TEXT          score literal text\n"
    "  --file PATH            score the contents of a file\n\n"
    "Options:\n"
    "  --model PATH           local llama.cpp-compatible GGUF (required)\n"
    "  --bos auto|always|never  beginning-of-stream policy (default: auto)\n"
    "  --context-size N       maximum tokens in the input (default: ";

constexpr std::string_view kUsageAfterContext =
    ")\n"
    "  --threads N            inference threads (default: hardware count)\n"
    "  --gpu-layers N         layers to offload; -1 means all (default: 0)\n"
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

void SetOption(Arguments& arguments, std::string_view option,
               std::string_view value) {
  if ((option == "--prompt" && arguments.file.has_value()) ||
      (option == "--file" && arguments.prompt.has_value())) {
    Usage("--prompt and --file are mutually exclusive");
  }
  if (option == "--model") {
    arguments.model = value;
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
    if (option == "--override-memory-check") {
      arguments.override_memory_check = true;
      continue;
    }
    if (i + 1 >= argc) {
      Usage(std::string(option) + " requires a value");
    }
    SetOption(arguments, option, argv[++i]);
  }
  if (arguments.model.empty()) {
    Usage("--model is required");
  }
  if (arguments.threads == 0) {
    arguments.threads = static_cast<std::int32_t>(
        std::max(1U, std::thread::hardware_concurrency()));
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

class Backend {
 public:
  Backend() { llama_backend_init(); }
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;
  ~Backend() { llama_backend_free(); }
};

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
}

std::optional<std::uint64_t> GpuAvailableMemory() {
  for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
    ggml_backend_dev_t device = ggml_backend_dev_get(i);
    if (ggml_backend_dev_type(device) != GGML_BACKEND_DEVICE_TYPE_GPU) {
      continue;
    }
    std::size_t free_bytes = 0;
    std::size_t total_bytes = 0;
    ggml_backend_dev_memory(device, &free_bytes, &total_bytes);
    return static_cast<std::uint64_t>(free_bytes);
  }
  return std::nullopt;
}

void CheckAvailableMemory(const Arguments& arguments) {
  if (arguments.override_memory_check) {
    return;
  }

  const std::uint64_t model_bytes = ModelFileSize(arguments.model);
  const std::optional<std::uint64_t> gpu_available =
      arguments.gpu_layers == 0 ? std::nullopt : GpuAvailableMemory();
  const std::optional<std::uint64_t> host_available = HostAvailableMemory();
  if (!host_available.has_value()) {
    if (arguments.gpu_layers != 0 && !gpu_available.has_value()) {
      const llmcc::MemoryCheckResult result = llmcc::CheckMemory(
          model_bytes, 0, gpu_available, arguments.gpu_layers, false);
      throw std::runtime_error(result.error);
    }
    std::cerr << "warning: could not read MemAvailable from /proc/meminfo; "
                 "skipping memory check\n";
    return;
  }

  const llmcc::MemoryCheckResult result = llmcc::CheckMemory(
      model_bytes, *host_available, gpu_available, arguments.gpu_layers, false);
  if (!result.ok) {
    throw std::runtime_error(result.error);
  }
}

using Model = std::unique_ptr<llama_model, decltype(&llama_model_free)>;
using Context = std::unique_ptr<llama_context, decltype(&llama_free)>;

void DiscardBackendLog(ggml_log_level level, const char* text,
                       void* user_data) {}

int Run(const Arguments& arguments, std::ostream& output,
        std::ostream& diagnostics,
        const std::optional<std::string>& input_override = std::nullopt) {
  const std::string input =
      input_override.has_value() ? *input_override : ReadInput(arguments);
  llama_log_set(DiscardBackendLog, nullptr);
  Backend backend;
  CheckAvailableMemory(arguments);

  llama_model_params model_parameters = llama_model_default_params();
  model_parameters.n_gpu_layers = arguments.gpu_layers;
  Model model(
      llama_model_load_from_file(arguments.model.c_str(), model_parameters),
      llama_model_free);
  if (!model) {
    throw std::runtime_error("could not load model: " +
                             arguments.model.string());
  }

  const llama_vocab* vocabulary = llama_model_get_vocab(model.get());
  const bool prepend_bos =
      arguments.bos == BosMode::kAlways ||
      (arguments.bos == BosMode::kAuto && llama_vocab_get_add_bos(vocabulary));
  std::vector<llama_token> tokens = Tokenize(vocabulary, input);
  if (prepend_bos) {
    const llama_token bos_token = llama_vocab_bos(vocabulary);
    if (bos_token < 0) {
      throw std::runtime_error("the model vocabulary has no BOS token");
    }
    tokens.insert(tokens.begin(), bos_token);
  }
  if (tokens.empty()) {
    diagnostics << "tokens=0 scored=0 mean_nll=null perplexity=null\n";
    return 0;
  }
  if (tokens.size() > arguments.context_size) {
    throw std::runtime_error(
        "input token count " + std::to_string(tokens.size()) +
        " exceeds --context-size " + std::to_string(arguments.context_size));
  }

  llama_context_params context_parameters = llama_context_default_params();
  // The option is an input limit, not a reason to reserve the entire KV cache
  // for short inputs. Size the actual context to this invocation so the large
  // default remains practical on memory-constrained accelerators.
  context_parameters.n_ctx = static_cast<std::uint32_t>(tokens.size());
  context_parameters.n_batch = 1;
  context_parameters.n_ubatch = 1;
  context_parameters.n_seq_max = 1;
  context_parameters.n_outputs_max = 1;
  context_parameters.n_outputs_max_per_seq = 1;
  context_parameters.n_threads = arguments.threads;
  context_parameters.n_threads_batch = arguments.threads;
  Context context(llama_init_from_model(model.get(), context_parameters),
                  llama_free);
  if (!context) {
    throw std::runtime_error("could not create inference context");
  }

  const std::size_t first_observed = prepend_bos ? 1 : 0;
  if (!prepend_bos) {
    const std::string piece = TokenPiece(vocabulary, tokens.front());
    WriteNullScore(output, 0, tokens.front(), piece, arguments.entropy);
  }

  const std::int32_t vocabulary_size = llama_vocab_n_tokens(vocabulary);
  double negative_log_likelihood = 0.0;
  std::size_t scored = 0;
  for (std::size_t source = 0; source + 1 < tokens.size(); ++source) {
    llama_token token = tokens[source];
    llama_pos position = static_cast<llama_pos>(source);
    std::int32_t sequence_count = 1;
    llama_seq_id sequence = 0;
    llama_seq_id* sequences = &sequence;
    std::int8_t output_logits = 1;
    llama_batch batch = {
        .n_tokens = 1,
        .token = &token,
        .embd = nullptr,
        .pos = &position,
        .n_seq_id = &sequence_count,
        .seq_id = &sequences,
        .logits = &output_logits,
    };
    const int decode_result = llama_decode(context.get(), batch);
    if (decode_result != 0) {
      throw std::runtime_error("llama_decode failed at token " +
                               std::to_string(source) + " with code " +
                               std::to_string(decode_result));
    }
    float* logits = llama_get_logits_ith(context.get(), 0);
    if (logits == nullptr) {
      throw std::runtime_error("model returned no logits");
    }

    const llama_token target = tokens[source + 1];
    if (target < 0 || target >= vocabulary_size) {
      throw std::runtime_error(
          "tokenizer produced a token outside the vocabulary");
    }
    const llmcc::TokenScore score = llmcc::ScoreToken(
        std::span<const float>(logits,
                               static_cast<std::size_t>(vocabulary_size)),
        static_cast<std::size_t>(target), arguments.entropy);
    const std::string piece = TokenPiece(vocabulary, target);
    WriteScore(output, source + 1 - first_observed, target, piece, score,
               arguments.entropy);
    negative_log_likelihood -= score.log_probability;
    ++scored;
  }

  if (scored == 0) {
    diagnostics << "tokens=" << (tokens.size() - first_observed)
                << " scored=0 mean_nll=null perplexity=null\n";
  } else {
    const double mean_nll =
        negative_log_likelihood / static_cast<double>(scored);
    diagnostics << std::setprecision(10)
                << "tokens=" << (tokens.size() - first_observed)
                << " scored=" << scored << " mean_nll=" << mean_nll
                << " perplexity=" << std::exp(mean_nll) << '\n';
  }
  return 0;
}

}  // namespace

namespace llmcc {

std::string ScoreEntropyJsonl(const std::filesystem::path& model,
                              std::string_view input,
                              const InferenceOptions& options) {
  Arguments arguments;
  arguments.model = model;
  arguments.context_size = options.context_size;
  arguments.gpu_layers = options.gpu_layers;
  arguments.entropy = true;
  arguments.threads = static_cast<std::int32_t>(
      std::max(1U, std::thread::hardware_concurrency()));
  std::ostringstream output;
  std::ostringstream diagnostics;
  static_cast<void>(Run(arguments, output, diagnostics, std::string(input)));
  return output.str();
}

int RunScoreCommand(int argc, char** argv) {
  try {
    return Run(ParseArguments(argc, argv), std::cout, std::cerr);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}

}  // namespace llmcc
