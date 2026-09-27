#ifndef LLM_CC_COMPARE_ENTROPY_TIER_H_
#define LLM_CC_COMPARE_ENTROPY_TIER_H_

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "src/analyze.h"
#include "src/compare/store.h"
#include "src/entropy_cache.h"

namespace llmcc::compare {

// Native entropy entries shared through a comparison store, one object per
// EntropyCacheKey at entropy/v2/<scorer>/<key>.cbor. Every worker that
// scores a file publishes its entry, so a later plan skips inference for
// unchanged bytes even when a scorer setting changes nothing but the result
// fingerprint. `scorer` names the plan's scorer identity: the key covers the
// model and inference settings but not the build, backend or host.
class StoreEntropyTier : public EntropyTier {
 public:
  StoreEntropyTier(compare::Store& store, std::string scorer)
      : store_(store), scorer_(std::move(scorer)) {}

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

  [[nodiscard]] std::string ObjectKey(std::string_view key) const {
    return "entropy/v2/" + scorer_ + "/" + std::string(key) + ".cbor";
  }

 private:
  compare::Store& store_;
  std::string scorer_;
};

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_ENTROPY_TIER_H_
