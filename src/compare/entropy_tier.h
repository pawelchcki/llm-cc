#ifndef LLM_CC_COMPARE_ENTROPY_TIER_H_
#define LLM_CC_COMPARE_ENTROPY_TIER_H_

#include <optional>
#include <string>
#include <string_view>

#include "src/analyze.h"
#include "src/compare/store.h"
#include "src/entropy_cache.h"

namespace llmcc::compare {

// Native entropy entries shared through a comparison store, one object per
// EntropyCacheKey at entropy/v2/<key>.cbor. Every worker that scores a file
// publishes its entry, so a later plan skips inference for unchanged bytes
// even when a scorer setting changes nothing but the result fingerprint.
class StoreEntropyTier : public EntropyTier {
 public:
  explicit StoreEntropyTier(compare::Store& store) : store_(store) {}

  std::optional<std::string> Fetch(std::string_view key) override {
    try {
      return store_.GetAtMost(ObjectKey(key), kMaxEntropyCacheEntryBytes);
    } catch (const StoreEntryTooLarge&) {
      return std::nullopt;  // No valid entry is this large.
    }
  }
  void Store(std::string_view key, std::string_view entry) override {
    store_.Put(ObjectKey(key), entry);
  }

  static std::string ObjectKey(std::string_view key) {
    return "entropy/v2/" + std::string(key) + ".cbor";
  }

 private:
  compare::Store& store_;
};

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_ENTROPY_TIER_H_
