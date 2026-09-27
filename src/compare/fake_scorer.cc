#include "src/compare/fake_scorer.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/analyze.h"
#include "src/model_identity.h"
#include "src/models.h"
#include "src/scoring_settings.h"

namespace llmcc::compare {
namespace {

class DeterministicEntropyProvider : public EntropyProvider {
 public:
  explicit DeterministicEntropyProvider(
      std::shared_ptr<std::atomic<int>> scored)
      : scored_(std::move(scored)) {}

  EntropyProviderResult Score(std::string_view source) override {
    if (scored_) {
      ++*scored_;
    }
    std::vector<EntropyRecord> records;
    constexpr std::size_t kTokenBytes = 3;
    for (std::size_t offset = 0; offset < source.size();
         offset += kTokenBytes) {
      const std::string_view bytes = source.substr(offset, kTokenBytes);
      unsigned int sum = 0;
      for (const char byte : bytes) {
        sum += static_cast<unsigned char>(byte);
      }
      const std::size_t position = records.size();
      records.push_back({.position = position,
                         .bytes = std::string(bytes),
                         .entropy = position == 0
                                        ? std::nullopt
                                        : std::optional<double>(
                                              static_cast<double>(
                                                  (sum + (position * 7)) % 97) /
                                              30.0)});
    }
    return {.records = std::move(records)};
  }

 private:
  std::shared_ptr<std::atomic<int>> scored_;
};

}  // namespace

ScorerSessionFactory DeterministicScorerFactory(
    std::shared_ptr<std::atomic<int>> scored) {
  return [scored = std::move(scored)](const ScorerRequest& request,
                                      ProgressReporter& progress) {
    static_cast<void>(progress);
    if (!request.model.model.has_value()) {
      throw std::invalid_argument("the deterministic scorer needs --model");
    }
    const ScoringSettings& settings = request.settings;
    ScorerSession session;
    session.backend = settings.backend == BackendKind::kAuto ? BackendKind::kCpu
                                                             : settings.backend;
    const bool device = settings.entropy_reduction == EntropyReduction::kDevice;
    session.entropy_cache = !request.no_cache;
    session.identity =
        InspectModel(*request.model.model, kFakeInferenceAbi,
                     BackendCacheIdentity(session.backend, settings.gpu_layers),
                     settings.context, settings.batch_size,
                     EntropyReductionName(settings.entropy_reduction),
                     device ? "device" : "host", session.entropy_cache,
                     FlashAttentionName(settings.flash_attention),
                     KvCacheTypeName(settings.kv_cache_type),
                     settings.kv_offload, request.require_model_digest);
    session.tau = ResolveTau(settings, request.model, &session.identity);
    session.analyzer = std::make_unique<ProjectAnalyzer>(
        ProjectAnalysisOptions{
            .model = session.identity,
            .tau_rule =
                settings.tau_percentile.has_value()
                    ? TauRule{.kind = TauRule::Kind::kPercentile,
                              .value = *settings.tau_percentile}
                    : TauRule{.kind = TauRule::Kind::kAbsolute,
                              .value = session.tau.value.value_or(kPaperTau)},
            .alpha = settings.alpha,
            .cache = session.entropy_cache,
            .hotspots = request.hotspots,
            .hierarchy_mode = settings.hierarchy_mode,
            .inference_context_tokens = settings.context,
            .tier = request.tier},
        [scored] {
          return std::make_unique<DeterministicEntropyProvider>(scored);
        });
    return session;
  };
}

}  // namespace llmcc::compare
