#ifndef LLM_CC_MODELS_H_
#define LLM_CC_MODELS_H_

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace llmcc {

struct ModelSpec {
  std::string_view name;
  std::string_view file;
  std::string_view url;
  std::uint64_t approx_bytes;
  std::string_view note;
  // Absolute entropy threshold used when --tau/--tau-percentile are absent.
  // The paper's 0.67 nats is a percentile of CodeLlama-7b's token entropies.
  // Each value is the same percentile of this model's pooled entropies on the
  // paper's HumanEval programs: the quantile at which the authors' CodeLlama
  // pipeline reaches exactly 0.67 there (62.48th). See
  // experiments/tau-calibration.
  std::optional<double> default_tau;
};

// The paper's threshold for CodeLlama-7b and the fallback for custom GGUFs.
inline constexpr double kPaperTau = 0.67;

inline constexpr std::array<ModelSpec, 6> kModels = {{
    {.name = "deepseek-coder-v2-lite-base-q6_k",
     .file = "DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf",
     .url = "https://huggingface.co/bartowski/"
            "DeepSeek-Coder-V2-Lite-Base-GGUF/resolve/"
            "babc004c32d74af2553661d912673f1d22dcb3f1/"
            "DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf",
     .approx_bytes = 14'000'000'000ULL,
     .note = "default",
     .default_tau = 0.5861},
    {.name = "deepseek-coder-6.7b-base-q6_k",
     .file = "deepseek-coder-6.7b-base.Q6_K.gguf",
     .url = "https://huggingface.co/TheBloke/"
            "deepseek-coder-6.7B-base-GGUF/resolve/"
            "65fb23ad31e6d8f90286d532bcf0fbcdae248ec4/"
            "deepseek-coder-6.7b-base.Q6_K.gguf",
     .approx_bytes = 5'500'000'000ULL,
     .note = "",
     .default_tau = 0.7970},
    {.name = "qwen2.5-coder-1.5b-q6_k",
     .file = "Qwen2.5-Coder-1.5B.Q6_K.gguf",
     .url = "https://huggingface.co/QuantFactory/"
            "Qwen2.5-Coder-1.5B-GGUF/resolve/"
            "73c3c696087d8d57a3005e31f2afe78f43059739/"
            "Qwen2.5-Coder-1.5B.Q6_K.gguf",
     .approx_bytes = 1'300'000'000ULL,
     .note = "smaller alternative: low memory",
     .default_tau = 0.7886},
    {.name = "qwen2.5-coder-3b-q6_k",
     .file = "Qwen2.5-Coder-3B-Q6_K.gguf",
     .url = "https://huggingface.co/bartowski/"
            "Qwen2.5-Coder-3B-GGUF/resolve/"
            "465c183318f1fcb5774394eee76f1b7f224494ec/"
            "Qwen2.5-Coder-3B-Q6_K.gguf",
     .approx_bytes = 2'500'000'000ULL,
     .note = "smaller alternative: closer rankings",
     .default_tau = 0.7783},
    {.name = "qwen2.5-coder-0.5b-q4_k_m",
     .file = "Qwen2.5-Coder-0.5B-Q4_K_M.gguf",
     .url = "https://huggingface.co/bartowski/"
            "Qwen2.5-Coder-0.5B-GGUF/resolve/"
            "01c1b8072277dfbfd0d051ec95b7a43bb23d2f6a/"
            "Qwen2.5-Coder-0.5B-Q4_K_M.gguf",
     .approx_bytes = 400'000'000ULL,
     .note = "smoke/testing",
     .default_tau = 0.9939},
    {.name = "codellama-7b-q8_0",
     .file = "CodeLlama-7b-hf.Q8_0.gguf",
     .url = "https://huggingface.co/mradermacher/CodeLlama-7b-hf-GGUF/resolve/"
            "af07f173257cbe75f416aaa14af160f301ccce92/"
            "CodeLlama-7b-hf.Q8_0.gguf",
     .approx_bytes = 7'200'000'000ULL,
     .note = "paper reference model; not default",
     .default_tau = 0.6761},
}};

const ModelSpec& DefaultModel();
const ModelSpec* FindModel(std::string_view name);
std::span<const ModelSpec> Models();
std::string FormatApproxSize(std::uint64_t bytes);

}  // namespace llmcc

#endif  // LLM_CC_MODELS_H_
