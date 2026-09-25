#include "src/scoring_settings.h"

#include <cmath>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/analysis_totals.h"
#include "src/model_identity.h"
#include "src/models.h"
#include "src/test_util.h"

namespace {

using llmcc::ScoringSettings;
using llmcc::test::Expect;
using llmcc::test::ExpectEq;

ScoringSettings Parse(
    const std::vector<std::pair<std::string_view, std::string_view>>& options) {
  ScoringSettings settings;
  for (const auto& [option, value] : options) {
    Expect(llmcc::ParseScoringOption(settings, option, value),
           "scoring option is recognized: " + std::string(option));
  }
  return settings;
}

std::string UsageMessage(const auto& function) {
  try {
    function();
  } catch (const llmcc::UsageError& error) {
    return error.what();
  }
  return {};
}

std::string Invalid(
    const std::vector<std::pair<std::string_view, std::string_view>>& options) {
  return UsageMessage([&] {
    ScoringSettings settings = Parse(options);
    llmcc::ValidateScoringSettings(settings);
  });
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  // Every flag reaches its setting.
  ScoringSettings parsed = Parse({{"--backend", "cpu"},
                                  {"--gpu-layers", "0"},
                                  {"--context", "4096"},
                                  {"--batch-size", "32"},
                                  {"--entropy-reduction", "host"},
                                  {"--flash-attn", "off"},
                                  {"--kv-cache-type", "f16"},
                                  {"--kv-offload", "off"},
                                  {"--hierarchy", "reference"},
                                  {"--tau", "0.5"},
                                  {"--alpha", "0.25"}});
  llmcc::ValidateScoringSettings(parsed);
  Expect(parsed.backend == llmcc::BackendKind::kCpu && parsed.gpu_layers == 0 &&
             parsed.context == 4096 && parsed.batch_size == 32 &&
             parsed.entropy_reduction == llmcc::EntropyReduction::kHost &&
             parsed.flash_attention == llmcc::FlashAttention::kOff &&
             parsed.kv_cache_type == llmcc::KvCacheType::kF16 &&
             !parsed.kv_offload &&
             parsed.hierarchy_mode == llmcc::HierarchyMode::kReference &&
             parsed.tau == 0.5 && parsed.alpha == 0.25,
         "every scoring flag reaches its setting");
  ScoringSettings unrelated;
  Expect(!llmcc::ParseScoringOption(unrelated, "--hotspots", "3") &&
             !llmcc::ParseScoringOption(unrelated, "--model", "x.gguf"),
         "output and model options are not scoring options");
  Expect(llmcc::ParseScoringFlag(unrelated, "--force-cpu") &&
             unrelated.force_cpu &&
             !llmcc::ParseScoringFlag(unrelated, "--no-cache"),
         "--force-cpu is the only valueless scoring flag");

  // Individually valid settings that no scorer run accepts are rejected.
  ExpectEq(Invalid({{"--flash-attn", "off"}}),
           std::string("quantized K/V cache requires --flash-attn on or auto"),
           "a quantized cache needs flash attention");
  ExpectEq(Invalid({{"--flash-attn", "off"}, {"--kv-cache-type", "f16"}}),
           std::string(), "an f16 cache works without flash attention");
  ExpectEq(Invalid({{"--tau", "1"}, {"--tau-percentile", "50"}}),
           std::string("--tau and --tau-percentile are mutually exclusive"),
           "absolute and percentile thresholds are exclusive");
  ExpectEq(Invalid({{"--tau", "-1"}}),
           std::string("--tau must be finite and non-negative"),
           "a negative threshold is rejected");
  ExpectEq(Invalid({{"--tau-percentile", "101"}}),
           std::string("--tau-percentile must be finite and between 0 and 100"),
           "a percentile above 100 is rejected");
  ExpectEq(Invalid({{"--alpha", "2"}}),
           std::string("--alpha must be finite and between 0 and 1"),
           "an alpha above one is rejected");
  ExpectEq(Invalid({{"--backend-dir", "/definitely/not/a/directory"}}),
           std::string("--backend-dir is not a directory: "
                       "/definitely/not/a/directory"),
           "a backend directory must exist");

  // The scorer parses --gpu-layers as int32 and refuses anything wider.
  ExpectEq(Invalid({{"--gpu-layers", "2147483647"}, {"--backend", "cpu"}}),
           Invalid({{"--gpu-layers", "2147483647"}, {"--backend", "cpu"}}),
           "validation is deterministic");
  ExpectEq(UsageMessage([] { Parse({{"--gpu-layers", "2147483648"}}); }),
           std::string("--gpu-layers expects a number, got '2147483648'"),
           "gpu layers beyond int32 are rejected");
  ExpectEq(UsageMessage([] { Parse({{"--gpu-layers", "-2"}}); }),
           std::string("--gpu-layers must be -1 or greater"),
           "gpu layers below -1 are rejected");
  ExpectEq(UsageMessage([] { Parse({{"--context", "0"}}); }),
           std::string("--context must be positive"),
           "an empty context is rejected");
  ExpectEq(UsageMessage([] { Parse({{"--kv-offload", "maybe"}}); }),
           std::string("--kv-offload expects on or off"),
           "an unknown offload mode is rejected");
  ExpectEq(UsageMessage([] { Parse({{"--backend", "tpu"}}); }).empty(), false,
           "an unknown backend is rejected");

  // Device reduction needs full offload; host reduction works everywhere.
  ScoringSettings partial = Parse({{"--backend", "rocm"},
                                   {"--gpu-layers", "20"},
                                   {"--entropy-reduction", "device"}});
  try {
    llmcc::CheckDeviceReduction(partial);
    Expect(false, "device reduction with partial offload is rejected");
  } catch (const std::invalid_argument& error) {
    ExpectEq(std::string(error.what()),
             std::string("device entropy reduction requires GPU execution"),
             "device reduction explains its requirement");
  }
  partial.entropy_reduction = llmcc::EntropyReduction::kHost;
  llmcc::CheckDeviceReduction(partial);

  // Cache identities name the execution, not how it was requested.
  ExpectEq(llmcc::BackendCacheIdentity(llmcc::BackendKind::kCpu, 0),
           std::string("cpu"), "CPU execution has one identity");
  ExpectEq(llmcc::BackendCacheIdentity(llmcc::BackendKind::kCuda, -1),
           std::string("cuda/gpu-layers=-1"), "GPU identities name offload");
  ExpectEq(llmcc::RequestedBackendCacheIdentity(ScoringSettings{}),
           std::string("cpu"), "automatic zero-layer execution is CPU");

  // Thresholds: explicit wins, then a registered calibration, then the paper.
  ScoringSettings defaults;
  ExpectEq(llmcc::ResolveTau(Parse({{"--tau", "0.9"}}), {}).value,
           std::optional(0.9), "an explicit tau wins");
  Expect(!llmcc::ResolveTau(Parse({{"--tau-percentile", "70"}}), {})
              .value.has_value(),
         "a percentile rule has no absolute tau");
  const auto custom =
      llmcc::ResolveTau(defaults, {.model = std::filesystem::path("x.gguf")});
  Expect(custom.value == llmcc::kPaperTau &&
             custom.source == std::string_view("paper-default"),
         "a custom model uses the paper threshold");
  const llmcc::ModelSpec& registered = llmcc::DefaultModel();
  if (registered.default_tau.has_value()) {
    ExpectEq(llmcc::ResolveTau(defaults, {}).value, registered.default_tau,
             "the default model supplies its calibration");
    llmcc::ModelIdentity other{};
    other.content_digest = std::string(64, '0');
    ExpectEq(llmcc::ResolveTau(defaults, {}, &other).source,
             std::string_view("model-digest-mismatch"),
             "other weights decline the calibration");
  }

  // Zero-token files add raw LM-CC and hierarchy counts, never token sums.
  llmcc::MetricTotals totals;
  llmcc::Accumulate(
      llmcc::FileTotals{
          .llm_cc = 2.5, .total_branch = 3, .total_comp_level = 4},
      totals);
  Expect(totals.llm_cc == 2.5 && totals.total_branch == 3 &&
             totals.total_comp_level == 4 && totals.token_count == 0 &&
             totals.lmcc == 0.0,
         "a zero-token file contributes raw LM-CC only");
  const auto zero = llmcc::TotalsMetricsJson(totals, "lmcc");
  Expect(zero["score"].is_null() && zero["lmcc_per_token"].is_null(),
         "token-normalized totals are null without tokens");
  ExpectEq(llmcc::TotalsMetricsJson(totals, "raw")["score"].get<double>(), 2.5,
           "raw totals stay defined without tokens");
  llmcc::Accumulate(llmcc::FileTotals{.llm_cc = 6.0,
                                      .total_branch = 1,
                                      .total_comp_level = 1,
                                      .token_count = 4,
                                      .high_entropy_tokens = 2,
                                      .entropy_sum = 3.0},
                    totals);
  const llmcc::Metrics metrics = llmcc::TotalMetrics(totals);
  Expect(metrics.lmcc == 8.5 && metrics.lmcc_per_token == 1.5 &&
             metrics.density == 0.5 && metrics.mean_entropy == 0.75,
         "per-token totals divide scored files only");

  // Option names round-trip.
  for (const auto reduction :
       {llmcc::EntropyReduction::kAuto, llmcc::EntropyReduction::kHost,
        llmcc::EntropyReduction::kDevice}) {
    ExpectEq(
        llmcc::ParseEntropyReduction(llmcc::EntropyReductionName(reduction)),
        std::optional(reduction), "entropy reductions round-trip");
  }
  Expect(!llmcc::ParseKvCacheType("q5_1").has_value() &&
             !llmcc::ParseFlashAttention("yes").has_value(),
         "unknown option names do not parse");
  return 0;
}
