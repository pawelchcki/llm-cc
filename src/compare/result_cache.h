#ifndef LLM_CC_COMPARE_RESULT_CACHE_H_
#define LLM_CC_COMPARE_RESULT_CACHE_H_

#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "src/compare/store.h"

namespace llmcc::compare {

// Per-file results in a store, each at results/v2/<key>.json inside an
// envelope {schema_version, stored_at, sha256, result} so a torn or edited
// entry reads as a miss rather than a wrong score.
class ResultCache {
 public:
  using Clock = std::function<std::chrono::system_clock::time_point()>;

  // A validated entry older than `refresh_days` is rewritten with a fresh
  // timestamp; one older than `expire_days`, or from the future, is a miss.
  explicit ResultCache(Store& store, int refresh_days = 20,
                       int expire_days = 30, Clock now = {});

  // The cached result for a plan item, or nullopt. Store failures throw.
  std::optional<nlohmann::json> Get(const nlohmann::json& item,
                                    const std::string& fingerprint);
  void Put(const nlohmann::json& result);

  static std::string ObjectKey(std::string_view key);

 private:
  Store& store_;
  int refresh_days_;
  int expire_days_;
  Clock now_;
};

// "YYYY-MM-DDTHH:MM:SSZ" in UTC.
std::string FormatTimestamp(std::chrono::system_clock::time_point time);
std::optional<std::chrono::system_clock::time_point> ParseTimestamp(
    std::string_view text);

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_RESULT_CACHE_H_
