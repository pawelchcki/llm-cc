#include "src/models.h"

#include <algorithm>

namespace llmcc {
namespace {

std::string FormatUnit(std::uint64_t bytes, std::uint64_t unit,
                       std::string_view suffix) {
  const std::uint64_t tenths = (bytes + unit / 20) / (unit / 10);
  std::string result = std::to_string(tenths / 10);
  if (tenths % 10 != 0) {
    result += '.';
    result += static_cast<char>('0' + tenths % 10);
  }
  return result + " " + std::string(suffix);
}

}  // namespace

const ModelSpec& DefaultModel() { return kModels.front(); }

const ModelSpec* FindModel(std::string_view name) {
  const auto model = std::ranges::find(kModels, name, &ModelSpec::name);
  return model == kModels.end() ? nullptr : &*model;
}

std::span<const ModelSpec> Models() { return kModels; }

std::string FormatApproxSize(std::uint64_t bytes) {
  constexpr std::uint64_t kKilobyte = 1'000;
  constexpr std::uint64_t kMegabyte = 1'000'000;
  constexpr std::uint64_t kGigabyte = 1'000'000'000;
  if (bytes >= kGigabyte) {
    return FormatUnit(bytes, kGigabyte, "GB");
  }
  if (bytes >= kMegabyte) {
    return FormatUnit(bytes, kMegabyte, "MB");
  }
  if (bytes >= kKilobyte) {
    return FormatUnit(bytes, kKilobyte, "KB");
  }
  return std::to_string(bytes) + " B";
}

}  // namespace llmcc
