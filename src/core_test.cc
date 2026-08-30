#include "src/core.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "src/test_util.h"

namespace {
using llmcc::test::Expect;
using llmcc::test::ExpectEq;

std::vector<llmcc::Token> ByteTokens(
    const std::vector<std::optional<double>>& entropies) {
  std::vector<llmcc::Token> tokens;
  tokens.reserve(entropies.size());
  for (std::size_t i = 0; i < entropies.size(); ++i) {
    tokens.push_back(
        {.start_byte = i, .end_byte = i + 1, .entropy = entropies[i]});
  }
  return tokens;
}
}  // namespace

int main() {
  Expect(!llmcc::Percentile({}, 67.0).has_value(), "empty percentile");
  const std::vector<double> pair = {0.0, 10.0};
  Expect(std::abs(llmcc::Percentile(pair, 67.0).value_or(-1.0) - 6.7) < 1e-12,
         "interpolated percentile");

  const auto tokens = ByteTokens({std::nullopt, 0.0, 10.0, 0.0});
  const std::vector<llmcc::StructuralEvent> events = {
      {.scope_start = 2, .byte_offset = 4, .depth = 1}};
  const llmcc::Analysis analysis = llmcc::Analyze(tokens, events, 67.0, 0.5);
  ExpectEq(analysis.total_branch, std::uint64_t{1}, "branch total");
  ExpectEq(analysis.total_comp_level, std::uint64_t{3}, "level total");
  ExpectEq(analysis.llm_cc, 2.0, "score");

  std::vector<llmcc::SemanticUnit> leaves;
  leaves.reserve(64);
  for (std::size_t i = 0; i < 64; ++i) {
    leaves.push_back({.start_byte = i, .end_byte = i + 1, .nesting_depth = i});
  }
  const auto hierarchy = llmcc::BuildHierarchy(leaves);
  ExpectEq(hierarchy.size(), std::size_t{1}, "one nested root");
  const llmcc::Unit* unit = &hierarchy.front();
  for (std::uint64_t level = 1; level <= 64; ++level) {
    ExpectEq(unit->level, level, "nested level");
    if (!unit->children.empty()) {
      unit = &unit->children.front();
    }
  }

  bool rejected = false;
  try {
    static_cast<void>(llmcc::Analyze(tokens, {}, 67.0, 1.1));
  } catch (const llmcc::AnalysisError&) {
    rejected = true;
  }
  Expect(rejected, "invalid alpha rejected");
  return 0;
}
