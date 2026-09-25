#ifndef LLM_CC_SCORING_SETTINGS_H_
#define LLM_CC_SCORING_SETTINGS_H_

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "src/backend.h"
#include "src/core.h"
#include "src/inference_options.h"

namespace llmcc {

struct ModelIdentity;

// A command line that cannot run; the caller prints it with the usage text.
class UsageError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

// Everything that changes an entropy or complexity result: shared by
// analysis and comparisons, and part of every cache identity.
struct ScoringSettings {
  BackendKind backend = BackendKind::kAuto;
  std::int32_t gpu_layers = 0;
  std::optional<std::int32_t> requested_gpu_layers;
  bool force_cpu = false;
  std::optional<std::filesystem::path> backend_directory;
  std::uint32_t context = kDefaultContextSize;
  std::uint32_t batch_size = kDefaultBatchSize;
  EntropyReduction entropy_reduction = EntropyReduction::kAuto;
  FlashAttention flash_attention = FlashAttention::kOn;
  KvCacheType kv_cache_type = KvCacheType::kQ8_0;
  bool kv_offload = true;
  HierarchyMode hierarchy_mode = HierarchyMode::kStructural;
  std::optional<double> tau;
  std::optional<double> tau_percentile;
  double alpha = 0.8;
};

template <typename Number>
Number ParseOptionNumber(std::string_view option, std::string_view value) {
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
    throw UsageError(std::string(option) + " expects a number, got '" +
                     std::string(value) + "'");
  }
  return result;
}

// Applies one scoring option that takes a value. Returns false for any other
// option and throws UsageError for an invalid value.
bool ParseScoringOption(ScoringSettings& settings, std::string_view option,
                        std::string_view value);
// Applies a valueless scoring flag, such as --force-cpu.
bool ParseScoringFlag(ScoringSettings& settings, std::string_view option);
// Resolves the requested backend and offload, applies LLM_CC_BACKEND_DIR,
// and rejects contradictory settings with a UsageError.
void ValidateScoringSettings(ScoringSettings& settings);

// Device reduction reads entropies on the GPU, so it needs full offload.
// Throws std::invalid_argument otherwise.
void CheckDeviceReduction(const ScoringSettings& settings);

bool ShouldFetchBackend(BackendKind backend, std::int32_t gpu_layers);
std::string BackendCacheIdentity(BackendKind backend, std::int32_t gpu_layers);
// The cache identity of the requested execution, before any device probe.
std::string RequestedBackendCacheIdentity(const ScoringSettings& settings);

struct ModelRequest {
  std::optional<std::filesystem::path> model;
  std::optional<std::string> model_name;
};

struct EffectiveTau {
  std::optional<double> value;
  std::string_view source;
};

// An explicit --tau or --tau-percentile wins. Otherwise a registered model
// supplies its calibrated 67th-percentile threshold, and custom GGUFs fall back
// to the paper's CodeLlama-7b value.
EffectiveTau ResolveTau(const ScoringSettings& settings,
                        const ModelRequest& model,
                        const ModelIdentity* identity = nullptr);

}  // namespace llmcc

#endif  // LLM_CC_SCORING_SETTINGS_H_
