#include "src/scoring_settings.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "src/model_identity.h"
#include "src/models.h"

namespace llmcc {
namespace {

void ApplyBackendDirectoryEnvironment(ScoringSettings& settings) {
  if (settings.backend_directory.has_value() ||
      !ShouldFetchBackend(settings.backend, settings.gpu_layers)) {
    return;
  }
  if (const char* environment = std::getenv("LLM_CC_BACKEND_DIR");
      environment != nullptr && *environment != '\0') {
    settings.backend_directory = environment;
  }
}

bool ParseExecutionOption(ScoringSettings& settings, std::string_view option,
                          std::string_view value) {
  if (option == "--gpu-layers") {
    settings.gpu_layers = ParseOptionNumber<std::int32_t>(option, value);
    settings.requested_gpu_layers = settings.gpu_layers;
    if (settings.gpu_layers < -1) {
      throw UsageError("--gpu-layers must be -1 or greater");
    }
    return true;
  }
  if (option == "--backend") {
    try {
      settings.backend = ParseBackend(value);
    } catch (const std::invalid_argument& error) {
      throw UsageError(error.what());
    }
    return true;
  }
  if (option == "--backend-dir") {
    settings.backend_directory = std::filesystem::u8path(value);
    return true;
  }
  if (option == "--context") {
    settings.context = ParseOptionNumber<std::uint32_t>(option, value);
    if (settings.context == 0) {
      throw UsageError("--context must be positive");
    }
    return true;
  }
  if (option == "--batch-size") {
    settings.batch_size = ParseOptionNumber<std::uint32_t>(option, value);
    if (settings.batch_size == 0) {
      throw UsageError("--batch-size must be positive");
    }
    return true;
  }
  if (option == "--entropy-reduction") {
    const auto reduction = ParseEntropyReduction(value);
    if (!reduction.has_value()) {
      throw UsageError("--entropy-reduction expects auto, host, or device");
    }
    settings.entropy_reduction = *reduction;
    return true;
  }
  if (option == "--flash-attn") {
    const auto flash_attention = ParseFlashAttention(value);
    if (!flash_attention.has_value()) {
      throw UsageError("--flash-attn expects auto, on, or off");
    }
    settings.flash_attention = *flash_attention;
    return true;
  }
  if (option == "--kv-cache-type") {
    const auto kv_cache_type = ParseKvCacheType(value);
    if (!kv_cache_type.has_value()) {
      throw UsageError("--kv-cache-type expects f16, q8_0, or q4_0");
    }
    settings.kv_cache_type = *kv_cache_type;
    return true;
  }
  if (option == "--kv-offload") {
    if (value == "on") {
      settings.kv_offload = true;
    } else if (value == "off") {
      settings.kv_offload = false;
    } else {
      throw UsageError("--kv-offload expects on or off");
    }
    return true;
  }
  return false;
}

}  // namespace

bool ParseScoringOption(ScoringSettings& settings, std::string_view option,
                        std::string_view value) {
  if (ParseExecutionOption(settings, option, value)) {
    return true;
  }
  if (option == "--hierarchy") {
    if (value == "structural") {
      settings.hierarchy_mode = HierarchyMode::kStructural;
    } else if (value == "reference") {
      settings.hierarchy_mode = HierarchyMode::kReference;
    } else {
      throw UsageError("--hierarchy expects structural or reference");
    }
    return true;
  }
  if (option == "--tau-percentile") {
    settings.tau_percentile = ParseOptionNumber<double>(option, value);
    return true;
  }
  if (option == "--tau") {
    settings.tau = ParseOptionNumber<double>(option, value);
    return true;
  }
  if (option == "--alpha") {
    settings.alpha = ParseOptionNumber<double>(option, value);
    return true;
  }
  return false;
}

bool ParseScoringFlag(ScoringSettings& settings, std::string_view option) {
  if (option == "--force-cpu") {
    settings.force_cpu = true;
    return true;
  }
  return false;
}

void ValidateScoringSettings(ScoringSettings& settings) {
  try {
    const auto execution = ResolveExecutionOptions(
        settings.backend, settings.requested_gpu_layers, settings.force_cpu);
    settings.backend = execution.backend;
    settings.gpu_layers = execution.gpu_layers;
  } catch (const std::invalid_argument& error) {
    throw UsageError(error.what());
  }
  ApplyBackendDirectoryEnvironment(settings);
  if (settings.backend_directory.has_value()) {
    std::error_code error;
    if (!std::filesystem::is_directory(*settings.backend_directory, error)) {
      throw UsageError("--backend-dir is not a directory: " +
                       settings.backend_directory->string());
    }
  }
  if (settings.tau.has_value() && settings.tau_percentile.has_value()) {
    throw UsageError("--tau and --tau-percentile are mutually exclusive");
  }
  if (settings.kv_cache_type != KvCacheType::kF16 &&
      settings.flash_attention == FlashAttention::kOff) {
    throw UsageError("quantized K/V cache requires --flash-attn on or auto");
  }
  if (settings.tau.has_value() &&
      (!std::isfinite(*settings.tau) || *settings.tau < 0.0)) {
    throw UsageError("--tau must be finite and non-negative");
  }
  if (settings.tau_percentile.has_value() &&
      (!std::isfinite(*settings.tau_percentile) ||
       *settings.tau_percentile < 0.0 || *settings.tau_percentile > 100.0)) {
    throw UsageError("--tau-percentile must be finite and between 0 and 100");
  }
  if (!std::isfinite(settings.alpha) || settings.alpha < 0.0 ||
      settings.alpha > 1.0) {
    throw UsageError("--alpha must be finite and between 0 and 1");
  }
  try {
    static_cast<void>(SelectBackend(settings.backend, settings.gpu_layers, {}));
  } catch (const std::invalid_argument& error) {
    throw UsageError(error.what());
  } catch (const std::runtime_error&) {  // NOLINT(bugprone-empty-catch)
    // Device availability is checked after the selected plugins are loaded.
  }
}

void CheckDeviceReduction(const ScoringSettings& settings) {
  if (settings.entropy_reduction == EntropyReduction::kDevice &&
      settings.gpu_layers != -1) {
    throw std::invalid_argument(
        "device entropy reduction requires GPU execution");
  }
}

bool ShouldFetchBackend(BackendKind backend, std::int32_t gpu_layers) {
  return backend == BackendKind::kCuda || backend == BackendKind::kRocm ||
         (backend == BackendKind::kAuto && gpu_layers != 0);
}

std::string BackendCacheIdentity(BackendKind backend, std::int32_t gpu_layers) {
  if (gpu_layers == 0 && backend == BackendKind::kCpu) {
    return "cpu";
  }
  return std::string(BackendName(backend)) +
         "/gpu-layers=" + std::to_string(gpu_layers);
}

std::string RequestedBackendCacheIdentity(const ScoringSettings& settings) {
  const BackendKind backend =
      settings.backend == BackendKind::kAuto && settings.gpu_layers == 0
          ? BackendKind::kCpu
          : settings.backend;
  return BackendCacheIdentity(backend, settings.gpu_layers);
}

EffectiveTau ResolveTau(const ScoringSettings& settings,
                        const ModelRequest& model,
                        const ModelIdentity* identity) {
  if (settings.tau_percentile.has_value()) {
    return {.value = std::nullopt, .source = "cli"};
  }
  if (settings.tau.has_value()) {
    return {.value = *settings.tau, .source = "cli"};
  }
  if (!model.model.has_value()) {
    const ModelSpec* spec = model.model_name.has_value()
                                ? FindModel(*model.model_name)
                                : &DefaultModel();
    if (spec != nullptr && spec->default_tau.has_value()) {
      // A cached file is accepted by name, so it may hold other weights than
      // the ones this threshold was calibrated on. Where the digest is known
      // and disagrees, the calibration is declined rather than misapplied.
      const bool mismatched =
          identity != nullptr && !identity->content_digest.empty() &&
          !spec->sha256.empty() && spec->sha256 != identity->content_digest;
      if (mismatched) {
        return {.value = kPaperTau, .source = "model-digest-mismatch"};
      }
      return {.value = *spec->default_tau, .source = "model-default"};
    }
  }
  return {.value = kPaperTau, .source = "paper-default"};
}

}  // namespace llmcc
