#ifndef LLM_CC_MODEL_IDENTITY_H_
#define LLM_CC_MODEL_IDENTITY_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace llmcc {

struct ModelIdentity {
  std::filesystem::path canonical_path;
  std::uint64_t size;
  std::int64_t modification_time;
  std::string inference_abi;
  std::string backend;
  std::uint32_t context_limit;
  std::uint32_t batch_size = 64;
  std::string reduction_policy = "auto";
  std::string effective_reducer = "host";
  std::string flash_attention = "on";
  std::string kv_cache_type = "q8_0";
  bool kv_offload = true;
  std::string content_digest;
};

ModelIdentity InspectModel(
    const std::filesystem::path& model, std::string_view inference_abi,
    std::string_view backend, std::uint32_t context_limit,
    std::uint32_t batch_size = 64, std::string_view reduction_policy = "auto",
    std::string_view effective_reducer = "host", bool cache_enabled = true,
    std::string_view flash_attention = "on",
    std::string_view kv_cache_type = "q8_0", bool kv_offload = true);

}  // namespace llmcc

#endif  // LLM_CC_MODEL_IDENTITY_H_
