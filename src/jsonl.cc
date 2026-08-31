#include "src/jsonl.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

namespace llmcc {
namespace {

std::uint8_t HexDigit(char byte) {
  if (byte >= '0' && byte <= '9') {
    return static_cast<std::uint8_t>(byte - '0');
  }
  if (byte >= 'a' && byte <= 'f') {
    return static_cast<std::uint8_t>(byte - 'a' + 10);
  }
  if (byte >= 'A' && byte <= 'F') {
    return static_cast<std::uint8_t>(byte - 'A' + 10);
  }
  throw std::invalid_argument("invalid hex digit");
}

std::string DecodeHex(std::string_view value) {
  if (value.size() % 2 != 0) {
    throw std::invalid_argument("hex string has odd length");
  }
  std::string result;
  result.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    result.push_back(
        static_cast<char>((HexDigit(value[i]) << 4) | HexDigit(value[i + 1])));
  }
  return result;
}

nlohmann::json UnitJson(const Unit& unit) {
  nlohmann::json children = nlohmann::json::array();
  for (const Unit& child : unit.children) {
    children.push_back(UnitJson(child));
  }
  return {{"start_byte", unit.start_byte},
          {"end_byte", unit.end_byte},
          {"level", unit.level},
          {"branching", unit.branching},
          {"children", std::move(children)}};
}

void MapUnit(Unit& unit, std::span<const std::size_t> map) {
  if (unit.start_byte >= map.size() || unit.end_byte >= map.size()) {
    throw std::out_of_range("unit is outside the preprocessing offset map");
  }
  unit.start_byte = map[unit.start_byte];
  unit.end_byte = map[unit.end_byte];
  for (Unit& child : unit.children) {
    MapUnit(child, map);
  }
}

}  // namespace

std::vector<EntropyRecord> ParseEntropyJsonl(std::string_view input) {
  std::vector<EntropyRecord> records;
  std::istringstream lines{std::string(input)};
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(lines, line)) {
    ++line_number;
    if (line.find_first_not_of(" \t\r") == std::string::npos) {
      continue;
    }
    nlohmann::json value;
    try {
      value = nlohmann::json::parse(line);
    } catch (const nlohmann::json::exception& error) {
      throw std::invalid_argument("invalid JSON on entropy line " +
                                  std::to_string(line_number) + ": " +
                                  error.what());
    }
    if (!value.is_object()) {
      throw std::invalid_argument(
          "entropy line " + std::to_string(line_number) + " is not an object");
    }
    if (!value.contains("position") ||
        !value["position"].is_number_unsigned()) {
      throw std::invalid_argument("entropy line " +
                                  std::to_string(line_number) +
                                  " has no valid position");
    }
    const std::uint64_t raw_position = value["position"].get<std::uint64_t>();
    if (raw_position > std::numeric_limits<std::size_t>::max()) {
      throw std::invalid_argument(
          "entropy position does not fit this platform");
    }
    const auto position = static_cast<std::size_t>(raw_position);
    if (position != records.size()) {
      throw std::invalid_argument(
          "entropy positions must be contiguous from zero; expected " +
          std::to_string(records.size()) + ", got " + std::to_string(position));
    }
    if (!value.contains("bytes_hex") || !value["bytes_hex"].is_string()) {
      throw std::invalid_argument("entropy line " +
                                  std::to_string(line_number) +
                                  " has no bytes_hex string");
    }
    if (!value.contains("entropy")) {
      throw std::invalid_argument("entropy line " +
                                  std::to_string(line_number) +
                                  " has no entropy field");
    }
    std::optional<double> entropy;
    if (!value["entropy"].is_null()) {
      if (!value["entropy"].is_number()) {
        throw std::invalid_argument(
            "entropy line " + std::to_string(line_number) + " is not numeric");
      }
      entropy = value["entropy"].get<double>();
      if (!std::isfinite(*entropy) || *entropy < 0.0) {
        throw std::invalid_argument("entropy line " +
                                    std::to_string(line_number) +
                                    " must be finite and non-negative");
      }
    } else if (position != 0) {
      throw std::invalid_argument("only the first token may have null entropy");
    }
    try {
      records.push_back(
          {.position = position,
           .bytes = DecodeHex(value["bytes_hex"].get<std::string>()),
           .entropy = entropy});
    } catch (const std::invalid_argument& error) {
      throw std::invalid_argument("invalid bytes_hex on entropy line " +
                                  std::to_string(line_number) + ": " +
                                  error.what());
    }
  }
  return records;
}

std::vector<Token> AlignTokens(std::string_view source,
                               std::span<const EntropyRecord> records) {
  std::size_t offset = 0;
  std::vector<Token> tokens;
  tokens.reserve(records.size());
  for (const EntropyRecord& record : records) {
    if (record.bytes.empty()) {
      throw std::invalid_argument("token " + std::to_string(record.position) +
                                  " has empty bytes_hex");
    }
    if (record.bytes.size() >
        std::numeric_limits<std::size_t>::max() - offset) {
      throw std::overflow_error("token byte offset overflow");
    }
    const std::size_t end = offset + record.bytes.size();
    if (end > source.size() || source.substr(offset, record.bytes.size()) !=
                                   std::string_view(record.bytes)) {
      throw std::invalid_argument(
          "token " + std::to_string(record.position) +
          " bytes do not match preprocessed source at byte " +
          std::to_string(offset));
    }
    tokens.push_back(
        {.start_byte = offset, .end_byte = end, .entropy = record.entropy});
    offset = end;
  }
  if (offset != source.size()) {
    throw std::invalid_argument("token bytes cover " + std::to_string(offset) +
                                " source bytes, expected " +
                                std::to_string(source.size()));
  }
  return tokens;
}

void MapAnalysisOffsets(Analysis& analysis, std::span<const std::size_t> map) {
  for (Unit& unit : analysis.units) {
    MapUnit(unit, map);
  }
}

nlohmann::json AnalysisJson(const Analysis& analysis) {
  nlohmann::json units = nlohmann::json::array();
  for (const Unit& unit : analysis.units) {
    units.push_back(UnitJson(unit));
  }
  return {{"llm_cc", analysis.llm_cc},
          {"total_branch", analysis.total_branch},
          {"total_comp_level", analysis.total_comp_level},
          {"alpha", analysis.alpha},
          {"tau", analysis.tau},
          {"units", std::move(units)}};
}

std::string PrettyAnalysisJson(const Analysis& analysis) {
  return AnalysisJson(analysis).dump(2) + '\n';
}

}  // namespace llmcc
