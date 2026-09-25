#ifndef LLM_CC_COMPARE_IDENTITY_H_
#define LLM_CC_COMPARE_IDENTITY_H_

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "src/scoring_settings.h"

namespace llmcc::compare {

inline constexpr int kSchemaVersion = 2;

struct ModelPin {
  std::string sha256;
  std::uint64_t bytes = 0;
};

// The running build, as every comparison stage must agree on it: version,
// stamped commit (or, unstamped, the executable's SHA-256), backend
// configuration, inference ABI and analysis version.
nlohmann::json ScorerJson(std::string_view inference_abi);

nlohmann::json ModelJson(const ModelPin& model);
ModelPin ModelFromJson(const nlohmann::json& model);

// Settings with auto values cannot name one exact result; comparisons need
// an explicit backend, entropy reduction and flash-attention mode.
void RequireExplicitScoring(const ScoringSettings& settings);

// The fingerprint names this build's own backends, so a plugin directory
// from --backend-dir or LLM_CC_BACKEND_DIR, which it cannot describe, is
// refused. Throws UsageError.
void RequireBuiltInBackends(const ScoringSettings& settings);

// An explicit threshold wins, then the calibration of a registered model
// with this digest, then the paper's value. Null for a percentile rule.
std::optional<double> ComparisonTau(const ScoringSettings& settings,
                                    std::string_view model_sha256);

// Everything that changes a file's result, in one canonical object.
nlohmann::json ScoringJson(const ScoringSettings& settings,
                           std::optional<double> tau);
// The validated settings a plan's scoring object describes.
ScoringSettings SettingsFromScoring(const nlohmann::json& scoring);

// sha256("llm-cc-compare-fingerprint-v2\0" + canonical({scorer, model,
// scoring})).
std::string Fingerprint(const nlohmann::json& scorer,
                        const nlohmann::json& model,
                        const nlohmann::json& scoring);

// sha256("llm-cc-compare-result-v2\0" + blob_id + "\0" + language + "\0" +
// fingerprint): one file's result under one fingerprint.
std::string ResultKey(std::string_view blob_id, std::string_view language,
                      std::string_view fingerprint);

bool IsHexDigest(std::string_view value, std::size_t length);

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_IDENTITY_H_
