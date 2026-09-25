#include "src/compare/result_cache.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "src/compare/identity.h"
#include "src/compare/json_util.h"
#include "src/compare/plan.h"
#include "src/sha256.h"

namespace llmcc::compare {
namespace {

using nlohmann::json;
using std::chrono::days;
using std::chrono::system_clock;

json Envelope(const json& result, system_clock::time_point stored_at) {
  return {{"schema_version", kSchemaVersion},
          {"stored_at", FormatTimestamp(stored_at)},
          {"sha256", Sha256Hex(CanonicalJson(result))},
          {"result", result}};
}

bool ParseField(std::string_view text, std::size_t offset, std::size_t size,
                int& value) {
  const char* begin = text.data() + offset;
  const auto [end, error] = std::from_chars(begin, begin + size, value);
  return error == std::errc{} && end == begin + size;
}

}  // namespace

std::string FormatTimestamp(system_clock::time_point time) {
  const auto day = std::chrono::floor<days>(time);
  const std::chrono::year_month_day date(day);
  const std::chrono::hh_mm_ss clock(
      std::chrono::floor<std::chrono::seconds>(time - day));
  std::array<char, 32> buffer{};
  const int size = std::snprintf(  // NOLINT(cppcoreguidelines-pro-type-vararg)
      buffer.data(), buffer.size(), "%04d-%02u-%02uT%02d:%02d:%02dZ",
      static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
      static_cast<unsigned>(date.day()),
      static_cast<int>(clock.hours().count()),
      static_cast<int>(clock.minutes().count()),
      static_cast<int>(clock.seconds().count()));
  return {buffer.data(), static_cast<std::size_t>(size)};
}

std::optional<system_clock::time_point> ParseTimestamp(std::string_view text) {
  if (text.size() != 20 || text[4] != '-' || text[7] != '-' ||
      text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
      text[19] != 'Z') {
    return std::nullopt;
  }
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!ParseField(text, 0, 4, year) || !ParseField(text, 5, 2, month) ||
      !ParseField(text, 8, 2, day) || !ParseField(text, 11, 2, hour) ||
      !ParseField(text, 14, 2, minute) || !ParseField(text, 17, 2, second) ||
      hour > 23 || minute > 59 || second > 59) {
    return std::nullopt;
  }
  const std::chrono::year_month_day date{
      std::chrono::year(year), std::chrono::month(static_cast<unsigned>(month)),
      std::chrono::day(static_cast<unsigned>(day))};
  if (!date.ok()) {
    return std::nullopt;
  }
  return std::chrono::sys_days(date) + std::chrono::hours(hour) +
         std::chrono::minutes(minute) + std::chrono::seconds(second);
}

ResultCache::ResultCache(Store& store, int refresh_days, int expire_days,
                         Clock now)
    : store_(store),
      refresh_days_(refresh_days),
      expire_days_(expire_days),
      now_(now ? std::move(now) : [] { return system_clock::now(); }) {
  if (refresh_days < 0 || expire_days < refresh_days) {
    throw std::invalid_argument(
        "cache retention must satisfy 0 <= refresh days <= expire days");
  }
}

std::string ResultCache::ObjectKey(std::string_view key) {
  return "results/v2/" + std::string(key) + ".json";
}

std::optional<json> ResultCache::Get(const json& item,
                                     const std::string& fingerprint) {
  const std::string key = item.at("key").get<std::string>();
  const std::optional<std::string> raw = store_.Get(ObjectKey(key));
  if (!raw.has_value()) {
    return std::nullopt;
  }
  json envelope;
  try {
    envelope = json::parse(*raw);
  } catch (const json::exception&) {
    return std::nullopt;
  }
  if (!envelope.is_object() || envelope.size() != 4 ||
      envelope.value("schema_version", json()) != kSchemaVersion ||
      !envelope.contains("result") || !envelope.contains("stored_at") ||
      !envelope["stored_at"].is_string() || !envelope.contains("sha256")) {
    return std::nullopt;
  }
  const json& result = envelope["result"];
  if (!ValidResult(result, item, fingerprint)) {
    return std::nullopt;
  }
  try {
    if (envelope["sha256"] != Sha256Hex(CanonicalJson(result))) {
      return std::nullopt;
    }
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  }
  const auto stored_at =
      ParseTimestamp(envelope["stored_at"].get_ref<const std::string&>());
  const system_clock::time_point now = now_();
  if (!stored_at.has_value() || *stored_at > now ||
      now - *stored_at > days(expire_days_)) {
    return std::nullopt;
  }
  if (now - *stored_at > days(refresh_days_)) {
    // Extends retention without changing provenance. A failing store fails
    // the run: that is broken infrastructure, not a miss.
    store_.Put(ObjectKey(key), CanonicalJson(Envelope(result, now)));
  }
  return result;
}

void ResultCache::Put(const json& result) {
  store_.Put(ObjectKey(result.at("key").get<std::string>()),
             CanonicalJson(Envelope(result, now_())));
}

}  // namespace llmcc::compare
