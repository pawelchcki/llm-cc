#ifndef LLM_CC_MODELS_H_
#define LLM_CC_MODELS_H_

#include <array>
#include <cstdint>
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
};

inline constexpr std::array<ModelSpec, 4> kModels = {{
    {.name = "deepseek-coder-v2-lite-base-q6_k",
     .file = "DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf",
     .url = "https://huggingface.co/bartowski/"
            "DeepSeek-Coder-V2-Lite-Base-GGUF/resolve/main/"
            "DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf",
     .approx_bytes = 14'000'000'000ULL,
     .note = "default"},
    {.name = "deepseek-coder-6.7b-base-q6_k",
     .file = "deepseek-coder-6.7b-base.Q6_K.gguf",
     .url = "https://huggingface.co/TheBloke/"
            "deepseek-coder-6.7B-base-GGUF/resolve/main/"
            "deepseek-coder-6.7b-base.Q6_K.gguf",
     .approx_bytes = 5'500'000'000ULL,
     .note = ""},
    {.name = "qwen2.5-coder-1.5b-q6_k",
     .file = "Qwen2.5-Coder-1.5B.Q6_K.gguf",
     .url = "https://huggingface.co/QuantFactory/"
            "Qwen2.5-Coder-1.5B-GGUF/resolve/main/"
            "Qwen2.5-Coder-1.5B.Q6_K.gguf",
     .approx_bytes = 1'200'000'000ULL,
     .note = ""},
    {.name = "qwen2.5-coder-0.5b-q4_k_m",
     .file = "Qwen2.5-Coder-0.5B-Q4_K_M.gguf",
     .url = "https://huggingface.co/bartowski/"
            "Qwen2.5-Coder-0.5B-GGUF/resolve/main/"
            "Qwen2.5-Coder-0.5B-Q4_K_M.gguf",
     .approx_bytes = 400'000'000ULL,
     .note = "smoke/testing"},
}};

const ModelSpec& DefaultModel();
const ModelSpec* FindModel(std::string_view name);
std::span<const ModelSpec> Models();
std::string FormatApproxSize(std::uint64_t bytes);

}  // namespace llmcc

#endif  // LLM_CC_MODELS_H_
