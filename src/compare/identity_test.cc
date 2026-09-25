#include "src/compare/identity.h"

#include <functional>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/compare/json_util.h"
#include "src/models.h"
#include "src/scoring_settings.h"
#include "src/test_util.h"

namespace {

using llmcc::ScoringSettings;
using llmcc::test::Expect;
using llmcc::test::ExpectEq;
using nlohmann::json;

ScoringSettings Settings(
    const std::vector<std::pair<std::string, std::string>>& options) {
  ScoringSettings settings;
  for (const auto& [option, value] : options) {
    llmcc::ParseScoringOption(settings, option, value);
  }
  llmcc::ParseScoringOption(settings, "--backend", "cpu");
  llmcc::ParseScoringOption(settings, "--entropy-reduction", "host");
  llmcc::ValidateScoringSettings(settings);
  return settings;
}

template <typename Exception>
bool Throws(const std::function<void()>& function) {
  try {
    function();
  } catch (const Exception&) {
    return true;
  }
  return false;
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  const json scorer = llmcc::compare::ScorerJson("abi");
  Expect(scorer["version"].is_string() && scorer["inference_abi"] == "abi" &&
             scorer["backend_configuration"].is_string() &&
             scorer["analysis_version"].is_number_integer(),
         "the scorer names its build");
  Expect(scorer["commit"].is_null() != scorer["executable"].is_null(),
         "a build is named by its commit, or unstamped by its executable");

  const llmcc::compare::ModelPin pin{.sha256 = std::string(64, 'a'),
                                     .bytes = 1234};
  const json model = llmcc::compare::ModelJson(pin);
  ExpectEq(llmcc::compare::ModelFromJson(model).bytes, std::uint64_t{1234},
           "model pins round-trip");
  Expect(Throws<std::invalid_argument>([] {
           static_cast<void>(llmcc::compare::ModelFromJson(
               {{"sha256", "ABC"}, {"bytes", 1}}));
         }),
         "model pins need a lowercase digest");

  // Every identity input changes the fingerprint.
  const ScoringSettings base = Settings({});
  const json scoring = llmcc::compare::ScoringJson(base, 0.67);
  const std::string reference =
      llmcc::compare::Fingerprint(scorer, model, scoring);
  std::set<std::string> fingerprints = {reference};
  for (const auto& [option, value] :
       std::vector<std::pair<std::string, std::string>>{
           {"--context", "4096"},
           {"--batch-size", "32"},
           {"--kv-cache-type", "f16"},
           {"--kv-offload", "off"},
           {"--hierarchy", "reference"},
           {"--alpha", "0.5"},
           {"--flash-attn", "off"}}) {
    std::vector<std::pair<std::string, std::string>> options = {
        {option, value}};
    if (option == "--flash-attn") {
      options.emplace_back("--kv-cache-type", "f16");
    }
    const json changed = llmcc::compare::ScoringJson(Settings(options), 0.67);
    Expect(
        fingerprints.insert(llmcc::compare::Fingerprint(scorer, model, changed))
            .second,
        "scoring " + option + " changes the fingerprint");
  }
  Expect(fingerprints
             .insert(llmcc::compare::Fingerprint(
                 scorer, model, llmcc::compare::ScoringJson(base, 0.5)))
             .second,
         "tau changes the fingerprint");
  for (const char* field :
       {"version", "commit", "executable", "backend_configuration",
        "inference_abi", "analysis_version"}) {
    json changed = scorer;
    changed[field] = "other";
    Expect(fingerprints
               .insert(llmcc::compare::Fingerprint(changed, model, scoring))
               .second,
           std::string("scorer ") + field + " changes the fingerprint");
  }
  for (const json& changed :
       {llmcc::compare::ModelJson(
            {.sha256 = std::string(64, 'b'), .bytes = 1234}),
        llmcc::compare::ModelJson({.sha256 = pin.sha256, .bytes = 1235})}) {
    Expect(fingerprints
               .insert(llmcc::compare::Fingerprint(scorer, changed, scoring))
               .second,
           "the model changes the fingerprint");
  }

  // Result keys bind content, language and fingerprint.
  const std::string key =
      llmcc::compare::ResultKey(std::string(40, '1'), "cpp", reference);
  Expect(llmcc::compare::IsHexDigest(key, 64), "result keys are digests");
  Expect(
      key != llmcc::compare::ResultKey(std::string(40, '1'), "c", reference) &&
          key != llmcc::compare::ResultKey(std::string(40, '2'), "cpp",
                                           reference) &&
          key != llmcc::compare::ResultKey(std::string(40, '1'), "cpp",
                                           std::string(64, '0')),
      "every key input changes the key");

  // Scoring round-trips through the plan and must be canonical and explicit.
  const ScoringSettings round_trip =
      llmcc::compare::SettingsFromScoring(scoring);
  ExpectEq(llmcc::compare::ScoringJson(round_trip, round_trip.tau), scoring,
           "plan scoring round-trips");
  json gpu = scoring;
  gpu["backend"] = "cuda/gpu-layers=-1";
  gpu["entropy_reduction"] = "device";
  const ScoringSettings cuda = llmcc::compare::SettingsFromScoring(gpu);
  Expect(cuda.backend == llmcc::BackendKind::kCuda && cuda.gpu_layers == -1,
         "GPU backends carry their offload");
  for (const auto& [field, value] :
       std::vector<std::pair<std::string, json>>{{"entropy_reduction", "auto"},
                                                 {"flash_attn", "auto"},
                                                 {"context", 0},
                                                 {"backend", "cuda"},
                                                 {"alpha", nullptr},
                                                 {"extra", 1}}) {
    json broken = scoring;
    broken[field] = value;
    Expect(Throws<std::invalid_argument>([&] {
             static_cast<void>(llmcc::compare::SettingsFromScoring(broken));
           }),
           "invalid plan scoring is rejected: " + field);
  }

  // Comparisons refuse auto values and settings no run accepts.
  ScoringSettings automatic;
  llmcc::ValidateScoringSettings(automatic);
  Expect(Throws<llmcc::UsageError>(
             [&] { llmcc::compare::RequireExplicitScoring(automatic); }),
         "an automatic backend is refused");
  automatic.backend = llmcc::BackendKind::kCpu;
  Expect(Throws<llmcc::UsageError>(
             [&] { llmcc::compare::RequireExplicitScoring(automatic); }),
         "automatic entropy reduction is refused");

  // Thresholds: explicit, then a registered calibration, then the paper.
  ExpectEq(llmcc::compare::ComparisonTau(Settings({{"--tau", "0.9"}}), ""),
           std::optional(0.9), "an explicit tau wins");
  ExpectEq(llmcc::compare::ComparisonTau(base, std::string(64, 'f')),
           std::optional(llmcc::kPaperTau), "unknown weights use the paper");
  for (const llmcc::ModelSpec& spec : llmcc::Models()) {
    if (!spec.sha256.empty() && spec.default_tau.has_value()) {
      ExpectEq(llmcc::compare::ComparisonTau(base, spec.sha256),
               spec.default_tau, "registered weights use their calibration");
    }
  }
  Expect(
      !llmcc::compare::ComparisonTau(Settings({{"--tau-percentile", "70"}}), "")
           .has_value(),
      "a percentile rule has no absolute tau");
  return 0;
}
