#include "src/compare/result_cache.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>

#include "src/analysis_totals.h"
#include "src/compare/identity.h"
#include "src/compare/json_util.h"
#include "src/compare/plan.h"
#include "src/compare/store.h"
#include "src/sha256.h"
#include "src/test_util.h"

namespace {

using llmcc::test::Expect;
using llmcc::test::ExpectEq;
using nlohmann::json;
using std::chrono::days;
using std::chrono::system_clock;

// A store that fails every write, to prove refresh failures surface.
class ReadOnlyStore : public llmcc::compare::Store {
 public:
  explicit ReadOnlyStore(llmcc::compare::Store& inner) : inner_(inner) {}
  std::optional<std::string> Get(std::string_view key) override {
    return inner_.Get(key);
  }
  void Put(std::string_view /*key*/, std::string_view /*contents*/) override {
    throw llmcc::compare::StoreError("read-only store");
  }
  [[nodiscard]] std::string Describe() const override { return "read-only"; }

 private:
  llmcc::compare::Store& inner_;
};

system_clock::time_point Time(std::string_view text) {
  const auto time = llmcc::compare::ParseTimestamp(text);
  Expect(time.has_value(), "fixture timestamps parse");
  return time.value_or(system_clock::time_point{});
}

json Stored(llmcc::compare::Store& store, std::string_view key) {
  const auto contents = store.Get(key);
  Expect(contents.has_value(), "the entry is stored");
  return json::parse(contents.value_or("null"));
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  const char* temporary = std::getenv("TEST_TMPDIR");
  Expect(temporary != nullptr, "TEST_TMPDIR is set");
  llmcc::compare::FilesystemStore store(std::filesystem::path(temporary) /
                                        "cache");
  system_clock::time_point now = Time("2026-09-01T12:00:00Z");
  const auto clock = [&now] { return now; };
  llmcc::compare::ResultCache cache(store, 20, 30, clock);

  const std::string fingerprint(64, 'f');
  const std::string blob_id(40, 'b');
  const json item = {
      {"key", llmcc::compare::ResultKey(blob_id, "cpp", fingerprint)},
      {"blob_id", blob_id},
      {"language", "cpp"},
      {"size", 10}};
  const json result = llmcc::compare::ResultRecord(item, fingerprint,
                                                   {.llm_cc = 4.5,
                                                    .total_branch = 2,
                                                    .total_comp_level = 3,
                                                    .token_count = 9,
                                                    .high_entropy_tokens = 1,
                                                    .entropy_sum = 2.25});
  Expect(llmcc::compare::ValidResult(result, item, fingerprint),
         "a result record is valid for its item");

  // Round trip; the envelope names the time and digest.
  Expect(!cache.Get(item, fingerprint).has_value(), "an empty cache misses");
  cache.Put(result);
  ExpectEq(cache.Get(item, fingerprint), std::optional(result),
           "a stored result is found");
  const std::string object =
      llmcc::compare::ResultCache::ObjectKey(item["key"].get<std::string>());
  ExpectEq(object.rfind("results/v2/", 0), std::size_t{0},
           "results live under results/v2");
  const json envelope = Stored(store, object);
  Expect(envelope["schema_version"] == 2 &&
             envelope["stored_at"] == "2026-09-01T12:00:00Z" &&
             envelope["result"] == result,
         "the envelope records version, time and result");

  // A different fingerprint or item never matches.
  Expect(!cache.Get(item, std::string(64, 'e')).has_value(),
         "another fingerprint misses");
  json other_language = item;
  other_language["language"] = "c";
  Expect(!cache.Get(other_language, fingerprint).has_value(),
         "another language misses");

  // Corruption reads as a miss.
  for (const std::string& corrupt :
       {std::string("not json"), std::string("{}"),
        [&] {
          json tampered = envelope;
          tampered["result"]["llm_cc"] = 99.0;
          return llmcc::compare::CanonicalJson(tampered);
        }(),
        [&] {
          json wrong = envelope;
          wrong["schema_version"] = 1;
          return llmcc::compare::CanonicalJson(wrong);
        }(),
        [&] {
          json negative = envelope;
          negative["result"]["llm_cc"] = -1.0;
          negative["sha256"] = llmcc::Sha256Hex(
              llmcc::compare::CanonicalJson(negative["result"]));
          return llmcc::compare::CanonicalJson(negative);
        }(),
        [&] {
          json garbled = envelope;
          garbled["stored_at"] = "yesterday";
          return llmcc::compare::CanonicalJson(garbled);
        }()}) {
    store.Put(object, corrupt);
    Expect(!cache.Get(item, fingerprint).has_value(),
           "a corrupt entry is a miss: " + corrupt.substr(0, 40));
  }

  // Retention: refresh after 20 days, expire after 30, never trust the future.
  cache.Put(result);
  now += days(21);
  ExpectEq(cache.Get(item, fingerprint), std::optional(result),
           "a 21-day-old entry still hits");
  Expect(Stored(store, object)["stored_at"] == "2026-09-22T12:00:00Z",
         "and is refreshed");
  now += days(31);
  Expect(!cache.Get(item, fingerprint).has_value(),
         "a 31-day-old entry has expired");
  cache.Put(result);
  now -= days(1);
  Expect(!cache.Get(item, fingerprint).has_value(),
         "an entry from the future is a miss");
  now += days(22);
  ReadOnlyStore read_only(store);
  llmcc::compare::ResultCache refreshing(read_only, 20, 30, clock);
  try {
    static_cast<void>(refreshing.Get(item, fingerprint));
    Expect(false, "a failed refresh fails the lookup");
  } catch (const llmcc::compare::StoreError&) {  // NOLINT(bugprone-empty-catch)
  }
  try {
    llmcc::compare::ResultCache invalid(store, 31, 30, clock);
    Expect(false, "refresh cannot exceed expiry");
  } catch (const std::invalid_argument&) {  // NOLINT(bugprone-empty-catch)
  }
  ExpectEq(llmcc::compare::FormatTimestamp(Time("2024-02-29T23:59:59Z")),
           std::string("2024-02-29T23:59:59Z"), "timestamps round-trip");
  Expect(!llmcc::compare::ParseTimestamp("2023-02-29T00:00:00Z").has_value(),
         "impossible dates are rejected");
  return 0;
}
