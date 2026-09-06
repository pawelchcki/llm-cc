#include "src/score_cmd.h"

#include <ggml-backend.h>
#include <ggml.h>
#include <llama.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
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

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#include "generated/version.h"
#include "src/backend.h"
#include "src/cache.h"
#include "src/download.h"
#include "src/inference_guard.h"
#include "src/models.h"
#include "src/progress.h"
#include "src/scoring.h"

namespace {

std::string Utf8Path(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::u8string value = path.u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
#else
  return path.string();
#endif
}
enum class BosMode : std::uint8_t { kAuto, kAlways, kNever };

struct Arguments {
  std::filesystem::path model;
  std::optional<std::string> model_name;
  std::optional<std::string> prompt;
  std::optional<std::filesystem::path> file;
  BosMode bos = BosMode::kAuto;
  std::uint32_t context_size = llmcc::kDefaultContextSize;
  std::int32_t threads = 0;
  std::int32_t gpu_layers = 0;
  std::optional<std::int32_t> requested_gpu_layers;
  bool force_cpu = false;
  bool assume_yes = false;
  llmcc::BackendKind backend = llmcc::BackendKind::kAuto;
  std::uint32_t batch_size = llmcc::kDefaultBatchSize;
  llmcc::EntropyReduction entropy_reduction = llmcc::EntropyReduction::kAuto;
  std::optional<std::filesystem::path> backend_directory;
  bool entropy = false;
  bool no_download = false;
  bool override_memory_check = false;
  std::string progress = "auto";
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
    "  --gpu-layers N         layers to offload; -1 means all (default: -1; 0 "
    "opts into CPU)\n"
    "  --force-cpu           explicitly use CPU with zero offload\n"
    "  -y, --assume-yes      accept missing model/backend downloads\n"
    "  --backend NAME         auto, cpu, cuda, or rocm (default: auto)\n"
    "  --batch-size N         decode rows per batch (default: 64)\n"
    "  --entropy-reduction M  auto, host, or device (default: auto)\n"
    "  --backend-dir DIR      GPU backend bundle/shared-library directory\n"
    "  --override-memory-check  bypass the preflight memory check\n"
    "  --progress M           auto, always, or never on stderr\n"
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

bool SetExecutionOption(Arguments& arguments, std::string_view option,
                        std::string_view value) {
  if (option == "--context-size") {
    arguments.context_size = ParseInteger<std::uint32_t>(option, value);
    if (arguments.context_size == 0) {
      Usage("--context-size must be positive");
    }
    return true;
  }
  if (option == "--threads") {
    arguments.threads = ParseInteger<std::int32_t>(option, value);
    if (arguments.threads < 1) {
      Usage("--threads must be positive");
    }
    return true;
  }
  if (option == "--batch-size") {
    arguments.batch_size = ParseInteger<std::uint32_t>(option, value);
    if (arguments.batch_size == 0) {
      Usage("--batch-size must be positive");
    }
    return true;
  }
  if (option == "--entropy-reduction") {
    if (value == "auto") {
      arguments.entropy_reduction = llmcc::EntropyReduction::kAuto;
    } else if (value == "host") {
      arguments.entropy_reduction = llmcc::EntropyReduction::kHost;
    } else if (value == "device") {
      arguments.entropy_reduction = llmcc::EntropyReduction::kDevice;
    } else {
      Usage("--entropy-reduction expects auto, host, or device");
    }
    return true;
  }
  if (option == "--gpu-layers") {
    arguments.gpu_layers = ParseInteger<std::int32_t>(option, value);
    arguments.requested_gpu_layers = arguments.gpu_layers;
    if (arguments.gpu_layers < -1) {
      Usage("--gpu-layers must be -1 or greater");
    }
    return true;
  }
  if (option == "--backend") {
    try {
      arguments.backend = llmcc::ParseBackend(value);
    } catch (const std::invalid_argument& error) {
      Usage(error.what());
    }
    return true;
  }
  if (option == "--backend-dir") {
    arguments.backend_directory = std::filesystem::u8path(value);
    return true;
  }
  return false;
}

void SetOption(Arguments& arguments, std::string_view option,
               std::string_view value) {
  if ((option == "--prompt" && arguments.file.has_value()) ||
      (option == "--file" && arguments.prompt.has_value())) {
    Usage("--prompt and --file are mutually exclusive");
  }
  if (SetExecutionOption(arguments, option, value)) {
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
  if (option == "--prompt") {
    arguments.prompt = value;
    return;
  }
  if (option == "--file") {
    arguments.file = std::filesystem::u8path(value);
    return;
  }
  if (option == "--bos") {
    if (value == "auto") {
      arguments.bos = BosMode::kAuto;
    } else if (value == "always") {
      arguments.bos = BosMode::kAlways;
    } else if (value == "never") {
      arguments.bos = BosMode::kNever;
    } else {
      Usage("--bos expects auto, always, or never");
    }
    return;
  }
  if (option == "--progress") {
    if (value != "auto" && value != "always" && value != "never")
      Usage("--progress expects auto, always, or never");
    arguments.progress = value;
    return;
  }
  Usage("unknown option: " + std::string(option));
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
    if (option == "--force-cpu") {
      arguments.force_cpu = true;
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
  try {
    const auto execution = llmcc::ResolveExecutionOptions(
        arguments.backend, arguments.requested_gpu_layers, arguments.force_cpu);
    arguments.backend = execution.backend;
    arguments.gpu_layers = execution.gpu_layers;
  } catch (const std::invalid_argument& error) {
    Usage(error.what());
  }
  if (arguments.entropy_reduction == llmcc::EntropyReduction::kDevice &&
      arguments.gpu_layers != -1) {
    Usage(
        "device entropy reduction requires full GPU offload (--gpu-layers -1)");
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
#if defined(_WIN32)
  DWORD console_mode = 0;
  if (!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &console_mode) &&
      _setmode(_fileno(stdin), _O_BINARY) == -1) {
    throw std::system_error(errno, std::generic_category(),
                            "cannot set stdin to binary mode");
  }
#endif
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
#ifdef _WIN32
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) {
    return std::nullopt;
  }
  return status.ullAvailPhys;
#elif defined(__APPLE__)
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
    llmcc::ReportWarning(*warning);
  }
  if (!use_gpu && llmcc::CliSessionActive()) {
    llmcc::ReportWarning("CPU inference can be slow. " +
                         llmcc::SmallerModelGuidance(HostAvailableMemory()));
  }
  if (arguments.override_memory_check) {
    return;
  }
  if (arguments.gpu_layers > 0) {
    llmcc::ReportWarning(
        "partial GPU offload memory use depends on model architecture; "
        "skipping memory check");
    return;
  }

  const std::uint64_t model_bytes = ModelFileSize(arguments.model);
  const std::optional<std::uint64_t> host_available = HostAvailableMemory();
  if (!host_available.has_value()) {
    if (use_gpu && !gpu_available.has_value()) {
      const llmcc::MemoryCheckResult result =
          llmcc::CheckMemory(model_bytes, 0, gpu_available, true, false);
      throw llmcc::GpuRecoverableError(result.error);
    }
    llmcc::ReportWarning(
        "could not determine available host memory; skipping memory check");
    return;
  }

  const llmcc::MemoryCheckResult result = llmcc::CheckMemory(
      model_bytes, *host_available, gpu_available, use_gpu, false);
  if (!result.ok) {
    if (use_gpu) throw llmcc::GpuRecoverableError(result.error);
    throw std::runtime_error(result.error + "\n" +
                             llmcc::SmallerModelGuidance(host_available));
  }
}

using Model = std::unique_ptr<llama_model, decltype(&llama_model_free)>;
using Context = std::unique_ptr<llama_context, decltype(&llama_free)>;
using Sampler = std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)>;

struct DeviceEntropyState {
  std::vector<ggml_tensor*> target_inputs;
  std::span<const llama_token> targets;
};

const char* DeviceEntropyName(const llama_sampler*) { return "llm-cc-entropy"; }

void DeviceEntropyApply(llama_sampler*, llama_token_data_array*) {}

bool DeviceEntropyBackendInit(llama_sampler*, ggml_backend_buffer_type_t,
                              std::uint32_t) {
  return true;
}

void DeviceEntropyBackendApply(llama_sampler* sampler, ggml_context* context,
                               ggml_cgraph*, llama_sampler_data* data) {
  auto* state = static_cast<DeviceEntropyState*>(sampler->ctx);
  ggml_tensor* logits =
      ggml_reshape_1d(context, data->logits, ggml_nelements(data->logits));
  ggml_tensor* logits_2d = ggml_reshape_2d(context, logits, 1, logits->ne[0]);
  ggml_tensor* maximum_index = ggml_argmax(context, logits);
  ggml_tensor* maximum = ggml_get_rows(context, logits_2d, maximum_index);
  maximum = ggml_reshape_1d(context, maximum, 1);
  ggml_tensor* shifted =
      ggml_sub(context, logits, ggml_repeat(context, maximum, logits));
  ggml_tensor* exponentials = ggml_exp(context, shifted);
  ggml_tensor* sum_exp = ggml_sum(context, exponentials);
  ggml_tensor* log_sum_exp = ggml_log(context, sum_exp);
  ggml_tensor* finite_shifted =
      ggml_clamp(context, shifted, std::numeric_limits<float>::lowest(), 0.0F);
  ggml_tensor* weighted =
      ggml_sum(context, ggml_mul(context, exponentials, finite_shifted));
  ggml_tensor* entropy =
      ggml_sub(context, log_sum_exp, ggml_div(context, weighted, sum_exp));

  ggml_tensor* target = ggml_new_tensor_1d(context, GGML_TYPE_I32, 1);
  ggml_set_input(target);
  state->target_inputs.push_back(target);
  ggml_tensor* shifted_2d =
      ggml_reshape_2d(context, shifted, 1, shifted->ne[0]);
  ggml_tensor* target_shifted = ggml_get_rows(context, shifted_2d, target);
  target_shifted = ggml_reshape_1d(context, target_shifted, 1);
  ggml_tensor* log_probability = ggml_sub(context, target_shifted, log_sum_exp);

  entropy = ggml_reshape_1d(context, entropy, 1);
  data->probs = ggml_concat(context, entropy, log_probability, 0);
  ggml_set_name(data->probs, "llm_cc_entropy_log_probability");
}

void DeviceEntropySetInput(llama_sampler* sampler) {
  auto* state = static_cast<DeviceEntropyState*>(sampler->ctx);
  for (std::size_t index = 0; index < state->target_inputs.size(); ++index) {
    const llama_token target =
        index < state->targets.size() ? state->targets[index] : 0;
    ggml_backend_tensor_set(state->target_inputs[index], &target, 0,
                            sizeof(target));
  }
}

void DeviceEntropyReset(llama_sampler* sampler) {
  static_cast<DeviceEntropyState*>(sampler->ctx)->target_inputs.clear();
}

llama_sampler_i kDeviceEntropySampler = {
    .name = DeviceEntropyName,
    .accept = nullptr,
    .apply = DeviceEntropyApply,
    .reset = nullptr,
    .clone = nullptr,
    .free = nullptr,
    .backend_init = DeviceEntropyBackendInit,
    .backend_accept = nullptr,
    .backend_apply = DeviceEntropyBackendApply,
    .backend_set_input = DeviceEntropySetInput,
    .backend_reset = DeviceEntropyReset,
    .copy_state = nullptr,
};

Sampler MakeDeviceEntropySampler(DeviceEntropyState& state) {
  Sampler chain(llama_sampler_chain_init(llama_sampler_chain_default_params()),
                llama_sampler_free);
  if (!chain) {
    throw std::runtime_error("could not create device entropy sampler");
  }
  llama_sampler_chain_add(chain.get(),
                          llama_sampler_init(&kDeviceEntropySampler, &state));
  return chain;
}

struct ScoreOptions {
  BosMode bos;
  std::uint32_t context_size;
  std::int32_t threads;
  bool entropy;
  std::uint32_t batch_size;
  bool device_reduction;
  std::string_view context_option;
  const std::function<void(std::size_t, std::size_t)>* progress = nullptr;
  bool* inference_started = nullptr;
};

class HostReductionPool {
 public:
  explicit HostReductionPool(std::size_t worker_count) {
    workers_.reserve(worker_count > 0 ? worker_count - 1 : 0);
    try {
      for (std::size_t worker = 1; worker < worker_count; ++worker) {
        workers_.emplace_back([this] { WorkerLoop(); });
      }
    } catch (...) {
      StopWorkers();
      workers_.clear();
      throw;
    }
  }

  HostReductionPool(const HostReductionPool&) = delete;
  HostReductionPool& operator=(const HostReductionPool&) = delete;

  ~HostReductionPool() { StopWorkers(); }

  void Reduce(std::span<float* const> logits_rows,
              std::span<const llama_token> targets, std::size_t vocabulary_size,
              bool entropy, std::span<llmcc::TokenScore> scores) {
    if (logits_rows.size() != targets.size() ||
        logits_rows.size() != scores.size()) {
      throw std::logic_error("host reduction row count mismatch");
    }
    {
      std::lock_guard lock(mutex_);
      logits_rows_ = logits_rows;
      targets_ = targets;
      scores_ = scores;
      vocabulary_size_ = vocabulary_size;
      entropy_ = entropy;
      worker_error_ = nullptr;
      workers_pending_ = workers_.size();
      next_row_.store(0, std::memory_order_relaxed);
      ++generation_;
    }
    task_ready_.notify_all();
    ReduceRows();

    std::exception_ptr error;
    {
      std::unique_lock lock(mutex_);
      task_done_.wait(lock, [this] { return workers_pending_ == 0; });
      error = worker_error_;
      logits_rows_ = {};
      targets_ = {};
      scores_ = {};
    }
    if (error) {
      std::rethrow_exception(error);
    }
  }

 private:
  void StopWorkers() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    task_ready_.notify_all();
    for (auto& worker : workers_) {
      worker.join();
    }
  }

  void WorkerLoop() {
    std::size_t observed_generation = 0;
    for (;;) {
      {
        std::unique_lock lock(mutex_);
        task_ready_.wait(lock, [this, &observed_generation] {
          return stopping_ || generation_ != observed_generation;
        });
        if (stopping_) {
          return;
        }
        observed_generation = generation_;
      }
      ReduceRows();
      {
        std::lock_guard lock(mutex_);
        --workers_pending_;
        if (workers_pending_ == 0) {
          task_done_.notify_one();
        }
      }
    }
  }

  void ReduceRows() {
    for (;;) {
      const std::size_t index =
          next_row_.fetch_add(1, std::memory_order_relaxed);
      if (index >= scores_.size()) {
        return;
      }
      try {
        scores_[index] = llmcc::ScoreToken(
            std::span<const float>(logits_rows_[index], vocabulary_size_),
            static_cast<std::size_t>(targets_[index]), entropy_);
      } catch (...) {
        std::lock_guard lock(mutex_);
        if (!worker_error_) {
          worker_error_ = std::current_exception();
        }
        return;
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable task_ready_;
  std::condition_variable task_done_;
  std::span<float* const> logits_rows_;
  std::span<const llama_token> targets_;
  std::span<llmcc::TokenScore> scores_;
  std::size_t vocabulary_size_ = 0;
  bool entropy_ = false;
  bool stopping_ = false;
  std::size_t generation_ = 0;
  std::size_t workers_pending_ = 0;
  std::atomic_size_t next_row_ = 0;
  std::exception_ptr worker_error_;
  // Apple SDKs before macOS 26 do not provide std::jthread. StopWorkers
  // joins these threads on both normal destruction and constructor failure.
  std::vector<std::thread> workers_;
};

void CheckBatchSize(std::uint32_t batch_size) {
  if (batch_size == 0) {
    throw std::invalid_argument("batch size must be positive");
  }
}

std::size_t HostReductionWorkerCount(std::uint32_t batch_size,
                                     std::int32_t threads) {
  return std::min<std::size_t>(batch_size, std::max<std::int32_t>(1, threads));
}

void WriteSummary(std::ostream* diagnostics, std::size_t tokens,
                  std::size_t scored, double negative_log_likelihood,
                  bool device_reduction) {
  if (diagnostics == nullptr) {
    return;
  }
  *diagnostics << "tokens=" << tokens << " scored=" << scored
               << " entropy_reducer=" << (device_reduction ? "device" : "host");
  if (scored == 0) {
    *diagnostics << " mean_nll=null perplexity=null\n";
    return;
  }
  const double mean_nll = negative_log_likelihood / static_cast<double>(scored);
  *diagnostics << std::setprecision(10) << " mean_nll=" << mean_nll
               << " perplexity=" << std::exp(mean_nll) << '\n';
}

void PrepareContext(llama_model* model, llmcc::BackendLogCapture& backend_log,
                    const ScoreOptions& options, std::uint32_t required,
                    Context& context, std::uint32_t& context_capacity,
                    Sampler& sampler, DeviceEntropyState& device_state) {
  if (context && context_capacity >= required) {
    llama_memory_clear(llama_get_memory(context.get()), true);
    return;
  }
  std::uint32_t capacity =
      context_capacity == 0 ? std::min(options.context_size,
                                       std::max(required, options.batch_size))
                            : context_capacity;
  while (capacity < required) {
    const std::uint64_t doubled = static_cast<std::uint64_t>(capacity) * 2;
    capacity = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(options.context_size, doubled));
  }
  context.reset();
  sampler.reset();
  llama_context_params parameters = llama_context_default_params();
  parameters.n_ctx = capacity;
  parameters.n_batch = options.batch_size;
  parameters.n_ubatch = options.batch_size;
  parameters.n_seq_max = 1;
  parameters.n_outputs_max = options.batch_size;
  parameters.n_outputs_max_per_seq = options.batch_size;
  parameters.n_threads = options.threads;
  parameters.n_threads_batch = options.threads;
  llama_sampler_seq_config sampler_config{};
  if (options.device_reduction) {
    sampler = MakeDeviceEntropySampler(device_state);
    sampler_config = {.seq_id = 0, .sampler = sampler.get()};
    parameters.samplers = &sampler_config;
    parameters.n_samplers = 1;
  }
  context.reset(llama_init_from_model(model, parameters));
  if (!context) {
    context_capacity = 0;
    const std::string detail = backend_log.Error();
    throw std::runtime_error("could not create inference context" +
                             (detail.empty() ? std::string() : ": " + detail));
  }
  context_capacity = capacity;
}

std::vector<llmcc::TokenScore> ScoreHostBatch(
    llama_context* context, std::span<const llama_token> targets,
    std::int32_t vocabulary_size, bool entropy, HostReductionPool& reduction) {
  std::vector<float*> logits_rows(targets.size());
  for (std::size_t index = 0; index < targets.size(); ++index) {
    logits_rows[index] =
        llama_get_logits_ith(context, static_cast<std::int32_t>(index));
    if (logits_rows[index] == nullptr) {
      throw std::runtime_error("model returned no logits");
    }
    if (targets[index] < 0 || targets[index] >= vocabulary_size) {
      throw std::runtime_error(
          "tokenizer produced a token outside the vocabulary");
    }
  }
  std::vector<llmcc::TokenScore> scores(targets.size());
  reduction.Reduce(logits_rows, targets,
                   static_cast<std::size_t>(vocabulary_size), entropy, scores);
  return scores;
}

llmcc::TokenScore ReadDeviceScore(llama_context* context, std::size_t index,
                                  bool entropy) {
  llmcc::TokenScore score{};
  const float* statistics =
      llama_get_sampled_probs_ith(context, static_cast<std::int32_t>(index));
  const std::uint32_t statistics_count = llama_get_sampled_probs_count_ith(
      context, static_cast<std::int32_t>(index));
  if (statistics == nullptr || statistics_count != 2 ||
      !std::isfinite(statistics[0]) || !std::isfinite(statistics[1]) ||
      statistics[0] < 0.0F) {
    throw std::runtime_error(
        "device entropy reduction returned invalid logits");
  }
  score.log_probability = statistics[1];
  score.probability = std::exp(score.log_probability);
  if (entropy) {
    score.entropy = statistics[0];
  }
  return score;
}

void ReportScoringProgress(const ScoreOptions& options, std::size_t scored,
                           std::size_t total) {
  llmcc::ReportCounter(scored, total, "tokens");
  if (options.progress != nullptr && *options.progress) {
    (*options.progress)(scored, total);
  }
}

void ScoreInput(llama_model* model, llmcc::BackendLogCapture& backend_log,
                std::string_view input, const ScoreOptions& options,
                std::ostream* output,
                std::vector<llmcc::EntropyRecord>* records,
                std::ostream* diagnostics, Context& context,
                std::uint32_t& context_capacity, Sampler& sampler,
                DeviceEntropyState& device_state,
                HostReductionPool* host_reduction) {
  CheckBatchSize(options.batch_size);
  if (!options.device_reduction && host_reduction == nullptr) {
    throw std::logic_error("host reduction requires a worker pool");
  }
  const llama_vocab* vocabulary = llama_model_get_vocab(model);
  const bool prepend_bos =
      options.bos == BosMode::kAlways ||
      (options.bos == BosMode::kAuto && llama_vocab_get_add_bos(vocabulary));
  llmcc::ReportPhase("tokenizing input");
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
    WriteSummary(diagnostics, 0, 0, 0.0, options.device_reduction);
    return;
  }
  if (!prepend_bos) {
    const std::string piece = TokenPiece(vocabulary, tokens.front());
    if (output != nullptr) {
      WriteNullScore(*output, 0, tokens.front(), piece, options.entropy);
    }
    if (records != nullptr) {
      records->push_back(
          {.position = 0, .bytes = piece, .entropy = std::nullopt});
    }
  }
  if (tokens.size() == 1) {
    WriteSummary(diagnostics, tokens.size() - first_observed, 0, 0.0,
                 options.device_reduction);
    return;
  }

  const std::size_t total_scored_tokens = tokens.size() - 1;
  if (options.inference_started != nullptr) {
    *options.inference_started = true;
  }
  llmcc::ReportPhase("inference");
  ReportScoringProgress(options, 0, total_scored_tokens);

  const std::size_t batch_size =
      std::min<std::size_t>(options.batch_size, tokens.size() - 1);
  PrepareContext(model, backend_log, options,
                 static_cast<std::uint32_t>(tokens.size()), context,
                 context_capacity, sampler, device_state);

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
    if (options.device_reduction) {
      device_state.targets =
          std::span<const llama_token>(tokens.data() + source + 1, count);
    }
    const int decode_result = llama_decode(context.get(), batch);
    if (decode_result != 0) {
      throw std::runtime_error("llama_decode failed at token " +
                               std::to_string(source) + " with code " +
                               std::to_string(decode_result));
    }
    std::vector<llmcc::TokenScore> host_scores;
    if (!options.device_reduction) {
      host_scores = ScoreHostBatch(
          context.get(),
          std::span<const llama_token>(tokens.data() + source + 1, count),
          vocabulary_size, options.entropy, *host_reduction);
    }
    for (std::size_t index = 0; index < count; ++index) {
      const std::size_t target_index = source + index + 1;
      const llama_token target = tokens[target_index];
      if (target < 0 || target >= vocabulary_size) {
        throw std::runtime_error(
            "tokenizer produced a token outside the vocabulary");
      }
      const llmcc::TokenScore score =
          options.device_reduction
              ? ReadDeviceScore(context.get(), index, options.entropy)
              : host_scores[index];
      const std::string piece = TokenPiece(vocabulary, target);
      if (output != nullptr) {
        WriteScore(*output, target_index - first_observed, target, piece, score,
                   options.entropy);
      }
      if (records != nullptr) {
        records->push_back({.position = target_index - first_observed,
                            .bytes = piece,
                            .entropy = score.entropy});
      }
      negative_log_likelihood -= score.log_probability;
      ++scored;
    }
    ReportScoringProgress(options, scored, total_scored_tokens);
  }
  WriteSummary(diagnostics, tokens.size() - first_observed, scored,
               negative_log_likelihood, options.device_reduction);
}

int Run(const Arguments& arguments, std::string_view input,
        const llmcc::BackendRuntime& backend,
        llmcc::BackendLogCapture& backend_log, std::ostream& output,
        std::ostream& diagnostics) {
  llmcc::InferenceGuard inference_guard(llmcc::BackendName(backend.selected()));
  const bool use_gpu = arguments.gpu_layers != 0;
  const std::optional<std::uint64_t> gpu_available =
      use_gpu ? GpuAvailableMemory() : std::nullopt;
  const bool device_available =
      llmcc::DeviceOutputGuaranteed(backend.selected(), arguments.gpu_layers,
                                    llmcc::CompiledBackend() == "metal");
  if (arguments.entropy_reduction == llmcc::EntropyReduction::kDevice &&
      !device_available) {
    const std::string message =
        "device entropy reduction requires GPU execution";
    if (use_gpu) throw llmcc::GpuRecoverableError(message);
    throw std::runtime_error(message);
  }
  const bool device_reduction =
      arguments.entropy_reduction == llmcc::EntropyReduction::kDevice ||
      (arguments.entropy_reduction == llmcc::EntropyReduction::kAuto &&
       device_available);
  CheckAvailableMemory(arguments, use_gpu, gpu_available);

  llama_model_params model_parameters = llama_model_default_params();
  model_parameters.n_gpu_layers = arguments.gpu_layers;
  const std::string model_path = Utf8Path(arguments.model);
  llmcc::ReportPhase("loading model");
  backend_log.Clear();
  Model model(llama_model_load_from_file(model_path.c_str(), model_parameters),
              llama_model_free);
  if (!model) {
    const std::string detail = backend_log.Error();
    const std::string message =
        "could not load model: " + arguments.model.string() +
        (detail.empty() ? std::string() : ": " + detail);
    if (use_gpu && llmcc::IsGpuAllocationFailure(detail)) {
      throw llmcc::GpuRecoverableError(message);
    }
    throw std::runtime_error(message);
  }
  backend_log.Clear();
  DeviceEntropyState device_state;
  Sampler sampler(nullptr, llama_sampler_free);
  Context context(nullptr, llama_free);
  std::uint32_t context_capacity = 0;
  std::optional<HostReductionPool> host_reduction;
  if (!device_reduction) {
    host_reduction.emplace(
        HostReductionWorkerCount(arguments.batch_size, arguments.threads));
  }
  bool inference_started = false;
  try {
    ScoreInput(model.get(), backend_log, input,
               {.bos = arguments.bos,
                .context_size = arguments.context_size,
                .threads = arguments.threads,
                .entropy = arguments.entropy,
                .batch_size = arguments.batch_size,
                .device_reduction = device_reduction,
                .context_option = "--context-size",
                .progress = nullptr,
                .inference_started = &inference_started},
               &output, nullptr, &diagnostics, context, context_capacity,
               sampler, device_state,
               host_reduction.has_value() ? &*host_reduction : nullptr);
  } catch (const std::exception& error) {
    if (use_gpu && inference_started)
      throw llmcc::GpuRecoverableError(error.what());
    throw;
  }
  return 0;
}

}  // namespace

namespace llmcc {

namespace {
std::unique_ptr<BackendRuntime> CreateBackendRuntime(
    const InferenceOptions& options) {
  try {
    return std::make_unique<BackendRuntime>(
        options.backend, options.gpu_layers, LLM_CC_VERSION,
        options.backend_directory, options.no_download,
        options.fetch_backend &&
            ShouldFetchBackend(options.backend, options.gpu_layers));
  } catch (const std::exception& error) {
    if (options.gpu_layers != 0) throw GpuRecoverableError(error.what());
    throw;
  }
}
}  // namespace

class EntropyScorer::Impl {
 public:
  Impl(const std::filesystem::path& model_path,
       const InferenceOptions& inference_options)
      : backend_(CreateBackendRuntime(inference_options)),
        inference_guard_(BackendName(backend_->selected())),
        model_(nullptr, llama_model_free),
        context_(nullptr, llama_free),
        context_limit_(inference_options.context_size),
        batch_size_(inference_options.batch_size),
        progress_(inference_options.progress),
        use_gpu_(inference_options.gpu_layers != 0),
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
    arguments.batch_size = batch_size_;
    const bool device_available = DeviceOutputGuaranteed(
        backend_->selected(), inference_options.gpu_layers,
        CompiledBackend() == "metal");
    if (inference_options.entropy_reduction == EntropyReduction::kDevice &&
        !device_available) {
      throw std::runtime_error(
          "device entropy reduction requires GPU execution");
    }
    device_reduction_ =
        inference_options.entropy_reduction == EntropyReduction::kDevice ||
        (inference_options.entropy_reduction == EntropyReduction::kAuto &&
         device_available);
    const bool use_gpu = inference_options.gpu_layers != 0;
    const auto gpu_available = use_gpu ? GpuAvailableMemory() : std::nullopt;
    CheckAvailableMemory(arguments, use_gpu, gpu_available);
    llama_model_params parameters = llama_model_default_params();
    parameters.n_gpu_layers = inference_options.gpu_layers;
    const std::string utf8_model_path = Utf8Path(model_path);
    ReportPhase("loading model");
    backend_log_.Clear();
    model_.reset(
        llama_model_load_from_file(utf8_model_path.c_str(), parameters));
    if (!model_) {
      const std::string detail = backend_log_.Error();
      const std::string message =
          "could not load model: " + model_path.string() +
          (detail.empty() ? std::string() : ": " + detail);
      if (use_gpu_ && IsGpuAllocationFailure(detail)) {
        throw GpuRecoverableError(message);
      }
      throw std::runtime_error(message);
    }
    backend_log_.Clear();
    if (!device_reduction_) {
      host_reduction_.emplace(HostReductionWorkerCount(batch_size_, threads_));
    }
  }

  std::string Score(std::string_view input) {
    std::ostringstream output;
    try {
      ScoreInput(model_.get(), backend_log_, input,
                 {.bos = BosMode::kAuto,
                  .context_size = context_limit_,
                  .threads = threads_,
                  .entropy = true,
                  .batch_size = batch_size_,
                  .device_reduction = device_reduction_,
                  .context_option = "--context",
                  .progress = &progress_},
                 &output, nullptr, nullptr, context_, context_capacity_,
                 sampler_, device_state_,
                 host_reduction_.has_value() ? &*host_reduction_ : nullptr);
    } catch (...) {
      context_.reset();
      sampler_.reset();
      context_capacity_ = 0;
      throw;
    }
    return output.str();
  }

  std::vector<EntropyRecord> ScoreRecords(std::string_view input) {
    std::vector<EntropyRecord> records;
    bool inference_started = false;
    try {
      ScoreInput(model_.get(), backend_log_, input,
                 {.bos = BosMode::kAuto,
                  .context_size = context_limit_,
                  .threads = threads_,
                  .entropy = true,
                  .batch_size = batch_size_,
                  .device_reduction = device_reduction_,
                  .context_option = "--context",
                  .progress = &progress_,
                  .inference_started = &inference_started},
                 nullptr, &records, nullptr, context_, context_capacity_,
                 sampler_, device_state_,
                 host_reduction_.has_value() ? &*host_reduction_ : nullptr);
    } catch (const std::exception& error) {
      context_.reset();
      sampler_.reset();
      context_capacity_ = 0;
      if (use_gpu_ && inference_started) {
        throw GpuRecoverableError(error.what());
      }
      throw;
    }
    return records;
  }

 private:
  BackendLogCapture backend_log_;
  std::unique_ptr<BackendRuntime> backend_;
  InferenceGuard inference_guard_;
  Model model_;
  DeviceEntropyState device_state_;
  Sampler sampler_{nullptr, llama_sampler_free};
  Context context_;
  std::optional<HostReductionPool> host_reduction_;
  std::uint32_t context_limit_;
  std::uint32_t batch_size_;
  std::function<void(std::size_t, std::size_t)> progress_;
  std::uint32_t context_capacity_ = 0;
  bool device_reduction_ = false;
  bool use_gpu_ = false;
  std::int32_t threads_;
};

EntropyScorer::EntropyScorer(const std::filesystem::path& model,
                             const InferenceOptions& options) {
  CheckBatchSize(options.batch_size);
  implementation_ = std::make_unique<Impl>(model, options);
}

EntropyScorer::~EntropyScorer() = default;
EntropyScorer::EntropyScorer(EntropyScorer&&) noexcept = default;
EntropyScorer& EntropyScorer::operator=(EntropyScorer&&) noexcept = default;

std::string EntropyScorer::Score(std::string_view input) {
  return implementation_->Score(input);
}

std::vector<EntropyRecord> EntropyScorer::ScoreRecords(std::string_view input) {
  return implementation_->ScoreRecords(input);
}

std::string_view EntropyReductionName(EntropyReduction reduction) {
  switch (reduction) {
    case EntropyReduction::kAuto:
      return "auto";
    case EntropyReduction::kHost:
      return "host";
    case EntropyReduction::kDevice:
      return "device";
  }
  return "unknown";
}

std::string_view InferenceAbi() {
  return "llama.cpp-c589f0ed10c643678c4707dd160c21ac7633ebc0/entropy-v2";
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
  bool gpu_requested = false;
  try {
    Arguments arguments = ParseArguments(argc, argv);
    gpu_requested = arguments.gpu_layers != 0;
    ProgressReporter progress(arguments.progress);
    CliSession session(progress, arguments.assume_yes, arguments.no_download);
    progress.Phase("reading scoring input");
    const std::string input = ReadInput(arguments);
    progress.Phase("resolving inference backend");
    BackendLogCapture capture;
    // Keep the preflight backend alive through model loading and inference.
    // Unloading it here would force another device probe and plugin load.
    BackendRuntime backend = [&] {
      try {
        return BackendRuntime(
            arguments.backend, arguments.gpu_layers, LLM_CC_VERSION,
            arguments.backend_directory, arguments.no_download,
            ShouldFetchBackend(arguments.backend, arguments.gpu_layers));
      } catch (const std::exception& error) {
        const auto detail = capture.Error();
        const std::string message =
            std::string(error.what()) +
            (detail.empty() ? std::string() : ": " + detail);
        if (gpu_requested) throw llmcc::GpuRecoverableError(message);
        throw std::runtime_error(message);
      }
    }();
    progress.Phase("resolving model");
    if (arguments.model_name.has_value()) {
      arguments.model =
          ResolveModel(std::nullopt, *FindModel(*arguments.model_name),
                       arguments.no_download, std::filesystem::current_path(),
                       CacheDir(), DownloadModel);
    }
    return Run(arguments, input, backend, capture, std::cout, std::cerr);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    if (gpu_requested &&
        dynamic_cast<const llmcc::GpuRecoverableError*>(&error)) {
      std::cerr << CpuRecoveryCommand(argc, argv, true) << '\n'
                << SmallerModelGuidance() << '\n';
    }
    return 1;
  }
}

}  // namespace llmcc
