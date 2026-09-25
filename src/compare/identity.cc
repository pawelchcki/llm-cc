#include "src/compare/identity.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "src/backend_fetch.h"
#include "src/build_info.h"
#include "src/compare/json_util.h"
#include "src/jsonl.h"
#include "src/models.h"
#include "src/sha256.h"

namespace llmcc::compare {
namespace {

using nlohmann::json;

json OptionalNumber(const std::optional<double>& value) {
  return value.has_value() ? json(*value) : json();
}

std::optional<double> NumberField(const json& scoring, const char* name) {
  const json& value = scoring.at(name);
  if (value.is_null()) {
    return std::nullopt;
  }
  if (!value.is_number()) {
    throw std::invalid_argument(std::string("scoring ") + name +
                                " must be a number");
  }
  return value.get<double>();
}

std::string TextField(const json& scoring, const char* name) {
  const json& value = scoring.at(name);
  if (!value.is_string()) {
    throw std::invalid_argument(std::string("scoring ") + name +
                                " must be a string");
  }
  return value.get<std::string>();
}

}  // namespace

bool IsHexDigest(std::string_view value, std::size_t length) {
  return value.size() == length &&
         std::ranges::all_of(value, [](char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

json ScorerJson(std::string_view inference_abi) {
  const std::string_view commit = build_info::GitSha();
  return {{"version", build_info::Version()},
          {"commit", commit.empty() ? json() : json(commit)},
          {"executable",
           commit.empty() ? json(RunningExecutableIdentity()) : json()},
          {"backend_configuration", build_info::BackendConfiguration()},
          {"inference_abi", inference_abi},
          {"analysis_version", kAnalysisVersion}};
}

void ValidateScorer(const json& scorer) {
  static constexpr std::array<const char*, 6> kFields = {
      "version",       "commit",          "executable", "backend_configuration",
      "inference_abi", "analysis_version"};
  if (!scorer.is_object() || scorer.size() != kFields.size() ||
      !std::ranges::all_of(
          kFields, [&](const char* field) { return scorer.contains(field); })) {
    throw std::invalid_argument(
        "scorer must have exactly version, commit, executable, "
        "backend_configuration, inference_abi and analysis_version");
  }
  const auto text = [&](const char* field) {
    return scorer[field].is_string() &&
           !scorer[field].get_ref<const std::string&>().empty();
  };
  if (!text("version") || !scorer["backend_configuration"].is_string() ||
      !text("inference_abi") ||
      !scorer["analysis_version"].is_number_integer()) {
    throw std::invalid_argument(
        "scorer version, backend_configuration, inference_abi and "
        "analysis_version have the wrong types");
  }
  // A stamped build names its commit; an unstamped one its own digest.
  const bool commit = text("commit");
  const bool executable =
      scorer["executable"].is_string() &&
      IsHexDigest(scorer["executable"].get_ref<const std::string&>(), 64);
  if (commit == executable || !(commit || scorer["commit"].is_null()) ||
      !(executable || scorer["executable"].is_null())) {
    throw std::invalid_argument(
        "scorer needs either a commit or an executable SHA-256, not both");
  }
}

json ModelJson(const ModelPin& model) {
  return {{"sha256", model.sha256}, {"bytes", model.bytes}};
}

ModelPin ModelFromJson(const json& model) {
  if (!model.is_object() || !model.contains("sha256") ||
      !model["sha256"].is_string() ||
      !IsHexDigest(model["sha256"].get_ref<const std::string&>(), 64) ||
      !model.contains("bytes") || !model["bytes"].is_number_unsigned() ||
      model["bytes"].get<std::uint64_t>() == 0) {
    throw std::invalid_argument(
        "model must pin a lowercase SHA-256 and a positive size");
  }
  return {.sha256 = model["sha256"], .bytes = model["bytes"]};
}

void RequireExplicitScoring(const ScoringSettings& settings) {
  if (settings.backend == BackendKind::kAuto) {
    throw UsageError("comparisons need an explicit --backend");
  }
  if (settings.entropy_reduction == EntropyReduction::kAuto) {
    throw UsageError(
        "comparisons need --entropy-reduction host or device, not auto");
  }
  if (settings.flash_attention == FlashAttention::kAuto) {
    throw UsageError("comparisons need --flash-attn on or off, not auto");
  }
  CheckDeviceReduction(settings);
}

void RequireBuiltInBackends(const ScoringSettings& settings) {
  if (settings.backend_directory.has_value()) {
    throw UsageError(
        "comparisons use this build's own backends; --backend-dir and "
        "LLM_CC_BACKEND_DIR are not part of the comparison fingerprint");
  }
}

std::optional<double> ComparisonTau(const ScoringSettings& settings,
                                    std::string_view model_sha256) {
  if (settings.tau_percentile.has_value()) {
    return std::nullopt;
  }
  if (settings.tau.has_value()) {
    return settings.tau;
  }
  for (const ModelSpec& spec : Models()) {
    if (spec.sha256 == model_sha256 && spec.default_tau.has_value()) {
      return spec.default_tau;
    }
  }
  return kPaperTau;
}

json ScoringJson(const ScoringSettings& settings, std::optional<double> tau) {
  return {
      {"backend", BackendCacheIdentity(settings.backend, settings.gpu_layers)},
      {"context", settings.context},
      {"batch_size", settings.batch_size},
      {"entropy_reduction", EntropyReductionName(settings.entropy_reduction)},
      {"flash_attn", FlashAttentionName(settings.flash_attention)},
      {"kv_cache_type", KvCacheTypeName(settings.kv_cache_type)},
      {"kv_offload", settings.kv_offload ? "on" : "off"},
      {"hierarchy", settings.hierarchy_mode == HierarchyMode::kStructural
                        ? "structural"
                        : "reference"},
      {"tau",
       settings.tau_percentile.has_value() ? json() : OptionalNumber(tau)},
      {"tau_percentile", OptionalNumber(settings.tau_percentile)},
      {"alpha", settings.alpha}};
}

ScoringSettings SettingsFromScoring(const json& scoring) {
  if (!scoring.is_object() || scoring.size() != 11) {
    throw std::invalid_argument("plan scoring has unexpected fields");
  }
  ScoringSettings settings;
  try {
    const std::string backend = TextField(scoring, "backend");
    if (backend == "cpu") {
      settings.backend = BackendKind::kCpu;
      settings.requested_gpu_layers = 0;
    } else {
      constexpr std::string_view kLayers = "/gpu-layers=";
      const std::size_t split = backend.find(kLayers);
      if (split == std::string::npos) {
        throw std::invalid_argument("plan scoring backend is malformed");
      }
      settings.backend = ParseBackend(backend.substr(0, split));
      settings.requested_gpu_layers = ParseOptionNumber<std::int32_t>(
          "backend", std::string_view(backend).substr(split + kLayers.size()));
    }
    const auto unsigned_field = [&](const char* name) {
      const json& value = scoring.at(name);
      if (!value.is_number_unsigned() || value.get<std::uint64_t>() == 0 ||
          value.get<std::uint64_t>() > UINT32_MAX) {
        throw std::invalid_argument(std::string("plan scoring ") + name +
                                    " must be a positive 32-bit integer");
      }
      return value.get<std::uint32_t>();
    };
    settings.context = unsigned_field("context");
    settings.batch_size = unsigned_field("batch_size");
    for (const auto& [option, field] :
         {std::pair{"--entropy-reduction", "entropy_reduction"},
          std::pair{"--flash-attn", "flash_attn"},
          std::pair{"--kv-cache-type", "kv_cache_type"},
          std::pair{"--kv-offload", "kv_offload"},
          std::pair{"--hierarchy", "hierarchy"}}) {
      ParseScoringOption(settings, option, TextField(scoring, field));
    }
    settings.tau = NumberField(scoring, "tau");
    settings.tau_percentile = NumberField(scoring, "tau_percentile");
    const std::optional<double> alpha = NumberField(scoring, "alpha");
    if (!alpha.has_value()) {
      throw std::invalid_argument("plan scoring needs alpha");
    }
    settings.alpha = *alpha;
    ValidateScoringSettings(settings);
    RequireExplicitScoring(settings);
  } catch (const nlohmann::json::exception& error) {
    throw std::invalid_argument(std::string("plan scoring is incomplete: ") +
                                error.what());
  } catch (const UsageError& error) {
    throw std::invalid_argument(std::string("plan scoring is invalid: ") +
                                error.what());
  }
  if (ScoringJson(settings, settings.tau) != scoring) {
    throw std::invalid_argument("plan scoring is not canonical");
  }
  return settings;
}

std::string Fingerprint(const json& scorer, const json& model,
                        const json& scoring) {
  std::string input = "llm-cc-compare-fingerprint-v2";
  input.push_back('\0');
  input += CanonicalJson(
      {{"scorer", scorer}, {"model", model}, {"scoring", scoring}});
  return Sha256Hex(input);
}

std::string ResultKey(std::string_view blob_id, std::string_view language,
                      std::string_view fingerprint) {
  std::string input = "llm-cc-compare-result-v2";
  input.push_back('\0');
  input += blob_id;
  input.push_back('\0');
  input += language;
  input.push_back('\0');
  input += fingerprint;
  return Sha256Hex(input);
}

}  // namespace llmcc::compare
