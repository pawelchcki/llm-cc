#include "src/analysis_session.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "src/build_info.h"
#include "src/cache.h"
#include "src/download.h"
#include "src/models.h"
#include "src/score_cmd.h"

namespace llmcc {
namespace {

class LlamaEntropyProvider : public EntropyProvider {
 public:
  LlamaEntropyProvider(const std::filesystem::path& cache_dir,
                       const std::filesystem::path& model,
                       const InferenceOptions& options)
      : scorer_((MarkCachedModelUsed(cache_dir, model), model), options) {}

  EntropyProviderResult Score(std::string_view source) override {
    auto result = scorer_.ScoreRecordsWithMetadata(source);
    return {
        .records = std::move(result.records),
        .metadata = {.source_tokens = result.metadata.source_tokens,
                     .inference_context_tokens = result.metadata.context_size,
                     .window_stride_tokens = result.metadata.window_stride,
                     .window_count = result.metadata.window_count}};
  }

 private:
  EntropyScorer scorer_;
};

BackendKind ResolveAnalysisBackend(const ScorerRequest& request,
                                   const ModelSpec& model_spec,
                                   ProgressReporter& progress) {
  const ScoringSettings& settings = request.settings;
  const bool fetch_backend =
      ShouldFetchBackend(settings.backend, settings.gpu_layers);
  // An explicit backend supplies an exact cache identity, so existing models
  // need a device check only on a cache miss.
  if (settings.backend != BackendKind::kAuto &&
      (request.model.model.has_value() ||
       std::filesystem::exists(CacheDir() / model_spec.file) ||
       std::filesystem::exists(std::filesystem::current_path() / "models" /
                               model_spec.file))) {
    progress.Phase(
        "configured backend=" + std::string(BackendName(settings.backend)) +
        " gpu_layers=" + std::to_string(settings.gpu_layers) +
        " (device checked on cache miss)");
    return settings.backend;
  }
  if (settings.gpu_layers == 0) {
    return BackendKind::kCpu;
  }
  BackendLogCapture backend_log;
  try {
    BackendRuntime runtime(settings.backend, settings.gpu_layers,
                           build_info::Version(), settings.backend_directory,
                           request.no_download, fetch_backend);
    return runtime.selected();
  } catch (const std::exception& error) {
    // The capture swallows the loader's diagnostics, so re-attach them to the
    // generic plugin error before unwinding.
    const std::string detail = backend_log.Error();
    throw GpuRecoverableError(std::string(error.what()) +
                              (detail.empty() ? std::string() : ": " + detail));
  }
}

}  // namespace

ScorerSession OpenScorerSession(const ScorerRequest& request,
                                ProgressReporter& progress) {
  const ScoringSettings& settings = request.settings;
  const bool fetch_backend =
      ShouldFetchBackend(settings.backend, settings.gpu_layers);
  progress.Phase("selecting inference backend");
  const ModelSpec& model_spec = request.model.model_name.has_value()
                                    ? *FindModel(*request.model.model_name)
                                    : DefaultModel();
  ScorerSession session;
  session.backend = ResolveAnalysisBackend(request, model_spec, progress);
  const std::filesystem::path model_cache = CacheDir();
  progress.Phase("resolving model");
  const auto resolved_model =
      ResolveModel(request.model.model, model_spec, request.no_download,
                   std::filesystem::current_path(), model_cache, DownloadModel);
  const bool device_available = DeviceOutputGuaranteed(
      session.backend, settings.gpu_layers, CompiledBackend() == "metal");
  if (settings.entropy_reduction == EntropyReduction::kDevice &&
      !device_available) {
    throw std::invalid_argument(
        "device entropy reduction requires GPU execution");
  }
  const bool device_reduction =
      settings.entropy_reduction == EntropyReduction::kDevice ||
      (settings.entropy_reduction == EntropyReduction::kAuto &&
       device_available);
  const std::string backend_identity =
      BackendCacheIdentity(session.backend, settings.gpu_layers);
  session.entropy_cache =
      !request.no_cache && !settings.backend_directory.has_value();
  progress.Phase("hashing model and resolving cache identity");
  // A calibrated tau belongs to specific weights, so this run needs the digest
  // even when the entropy cache is off and nothing else would compute it.
  const bool calibrated_tau_applies =
      !settings.tau.has_value() && !settings.tau_percentile.has_value() &&
      !request.model.model.has_value() && model_spec.default_tau.has_value();
  session.identity = InspectModel(
      resolved_model, InferenceAbi(), backend_identity, settings.context,
      settings.batch_size, EntropyReductionName(settings.entropy_reduction),
      device_reduction ? "device" : "host", session.entropy_cache,
      FlashAttentionName(settings.flash_attention),
      KvCacheTypeName(settings.kv_cache_type), settings.kv_offload,
      calibrated_tau_applies);
  session.tau = ResolveTau(settings, request.model, &session.identity);

  const BackendKind resolved_backend = session.backend;
  const bool backend_diagnostics = request.backend_diagnostics;
  const bool no_download = request.no_download;
  const std::filesystem::path model_path = session.identity.canonical_path;
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
          .inference_context_tokens = settings.context},
      [&progress, settings, resolved_backend, backend_diagnostics, no_download,
       fetch_backend, model_cache, model_path]() {
        progress.Phase("loading model after entropy cache miss");
        return std::make_unique<LlamaEntropyProvider>(
            model_cache, model_path,
            InferenceOptions{
                .context_size = settings.context,
                .gpu_layers = settings.gpu_layers,
                .backend = resolved_backend,
                .batch_size = settings.batch_size,
                .entropy_reduction = settings.entropy_reduction,
                .flash_attention = settings.flash_attention,
                .kv_cache_type = settings.kv_cache_type,
                .kv_offload = settings.kv_offload,
                .backend_diagnostics = backend_diagnostics,
                .progress =
                    [&progress](std::size_t completed, std::size_t total) {
                      progress.Tokens(completed, total);
                    },
                .backend_directory = settings.backend_directory,
                .no_download = no_download,
                .fetch_backend = fetch_backend});
      });
  return session;
}

}  // namespace llmcc
