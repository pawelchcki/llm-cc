#ifndef LLM_CC_CORE_H_
#define LLM_CC_CORE_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace llmcc {

struct Token {
  std::size_t start_byte;
  std::size_t end_byte;
  std::optional<double> entropy;
  bool operator==(const Token&) const = default;
};

struct StructuralEvent {
  std::size_t scope_start;
  std::size_t byte_offset;
  std::size_t depth;
  bool operator==(const StructuralEvent&) const = default;
};

struct Unit {
  std::size_t start_byte;
  std::size_t end_byte;
  std::uint64_t level;
  std::uint64_t branching;
  std::vector<Unit> children;
  bool operator==(const Unit&) const = default;
};

struct Analysis {
  double llm_cc;
  std::uint64_t total_branch;
  std::uint64_t total_comp_level;
  double alpha;
  double tau;
  std::vector<Unit> units;
  bool operator==(const Analysis&) const = default;
};

struct SemanticUnit {
  std::size_t start_byte;
  std::size_t end_byte;
  std::size_t nesting_depth;
  bool operator==(const SemanticUnit&) const = default;
};

class AnalysisError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

std::optional<double> Percentile(std::span<const double> values,
                                 double percentile);
std::pair<double, std::vector<SemanticUnit>> DetectSemanticUnits(
    std::span<const Token> tokens,
    std::span<const StructuralEvent> structural_events, double tau_percentile);
std::vector<Unit> BuildHierarchy(std::span<const SemanticUnit> semantic_units);
Analysis Analyze(std::span<const Token> tokens,
                 std::span<const StructuralEvent> structural_events,
                 double tau_percentile = 67.0, double alpha = 0.8);

}  // namespace llmcc

#endif  // LLM_CC_CORE_H_
