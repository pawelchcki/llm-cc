#ifndef LLM_CC_MODEL_IDENTITY_H_
#define LLM_CC_MODEL_IDENTITY_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace llmcc {

// Model digest memo.
//
// Hashing a multi-gigabyte GGUF model on every invocation is wasteful when the
// file has not changed since it was last hashed. `InspectModel` therefore reads
// and writes an advisory memo at
//
//   <entropy cache directory>/model-digests/<key>.json
//
// where <key> is the lowercase hex SHA-256 of the UTF-8 bytes of the model's
// canonical path in generic form. The directory is private (0700) and files are
// written atomically. The JSON object is
//
//   {"format": "llm-cc-model-digest-memo-v1",
//    "size": <bytes>, "mtime": <time>, "ctime": <time>,
//    "device": <st_dev>, "inode": <st_ino>,
//    "digest": "<64 lowercase hex>"}
//
// On POSIX the two times are nanoseconds since the epoch (`st_mtim`/`st_ctim`)
// and the identifiers come from `st_dev`/`st_ino`; Windows uses its own file
// time units, volume serial number and file index. All five stat fields must
// equal the file's current `stat()` both when the memo is read and again
// afterwards, or the model is hashed as usual. Unknown keys are ignored, so an
// older reader accepts a memo carrying "format". The memo is advisory: a
// missing, unreadable or stale memo only costs a hash. A writer must never
// record a digest it has not itself computed from the file it describes. For a
// split GGUF model the memo describes one shard file; the companion shards are
// hashed and memoized individually.

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
    std::string_view kv_cache_type = "q8_0", bool kv_offload = true,
    // Computes `content_digest` even when the entropy cache is disabled, for
    // callers that must know which weights they resolved.
    bool digest_required = false);

}  // namespace llmcc

#endif  // LLM_CC_MODEL_IDENTITY_H_
