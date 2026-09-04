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

int main() {  // NOLINT(bugprone-exception-escape)
  Expect(!llmcc::Percentile({}, 67.0).has_value(), "empty percentile");
  const std::vector<double> pair = {0.0, 10.0};
  Expect(std::abs(llmcc::Percentile(pair, 67.0).value_or(-1.0) - 6.7) < 1e-12,
         "interpolated percentile");

  const auto tokens = ByteTokens({std::nullopt, 0.0, 10.0, 0.0});
  const std::vector<llmcc::StructuralEvent> events = {
      {.scope_start = 2, .byte_offset = 4, .depth = 1}};
  const llmcc::Analysis analysis = llmcc::Analyze(
      tokens, events, {},
      {.kind = llmcc::TauRule::Kind::kPercentile, .value = 67.0}, 0.5);
  ExpectEq(analysis.total_branch, std::uint64_t{2}, "branch total");
  ExpectEq(analysis.total_comp_level, std::uint64_t{6}, "level total");
  ExpectEq(analysis.llm_cc, 4.0, "score");

  std::vector<llmcc::SemanticUnit> leaves;
  leaves.reserve(64);
  for (std::size_t i = 0; i < 64; ++i) {
    leaves.push_back({.start_byte = i, .end_byte = i + 1, .nesting_depth = i});
  }
  const auto hierarchy = llmcc::BuildHierarchy(leaves);
  ExpectEq(hierarchy.size(), std::size_t{1}, "one nested root");
  const llmcc::Unit* unit = &hierarchy.front();
  for (std::uint64_t level = 2; level <= 65; ++level) {
    ExpectEq(unit->level, level, "nested level");
    if (!unit->children.empty()) {
      unit = &unit->children.front();
    }
  }

  bool rejected = false;
  try {
    static_cast<void>(llmcc::Analyze(
        tokens, {}, {},
        {.kind = llmcc::TauRule::Kind::kPercentile, .value = 67.0}, 1.1));
  } catch (const llmcc::AnalysisError&) {
    rejected = true;
  }
  Expect(rejected, "invalid alpha rejected");

  const auto absolute = llmcc::Analyze(
      tokens, {}, {}, {.kind = llmcc::TauRule::Kind::kAbsolute, .value = 0.67});
  const auto percentile = llmcc::Analyze(
      tokens, {}, {},
      {.kind = llmcc::TauRule::Kind::kPercentile, .value = 50.0});
  ExpectEq(absolute.tau, 0.67, "absolute tau");
  ExpectEq(percentile.tau, 0.0, "percentile tau");
  ExpectEq(absolute.tau_rule.kind, llmcc::TauRule::Kind::kAbsolute,
           "absolute rule retained");
  ExpectEq(percentile.tau_rule.kind, llmcc::TauRule::Kind::kPercentile,
           "percentile rule retained");

  const std::vector<llmcc::Token> same_line = {
      {.start_byte = 0, .end_byte = 1, .entropy = std::nullopt},
      {.start_byte = 2, .end_byte = 3, .entropy = 1.0},
      {.start_byte = 3, .end_byte = 4, .entropy = 2.0},
  };
  const std::vector<std::size_t> two_lines = {0, 2};
  const auto [same_line_tau, same_line_units] = llmcc::DetectSemanticUnits(
      same_line, {}, two_lines,
      {.kind = llmcc::TauRule::Kind::kAbsolute, .value = 1.0});
  ExpectEq(same_line_tau, 1.0, "line-snapped tau");
  ExpectEq(same_line_units.size(), std::size_t{2},
           "two hot tokens on one line make one boundary");

  const std::vector<llmcc::Token> straddling = {
      {.start_byte = 0, .end_byte = 1, .entropy = std::nullopt},
      {.start_byte = 1, .end_byte = 3, .entropy = 1.0},
      {.start_byte = 3, .end_byte = 4, .entropy = 0.0},
  };
  const auto [straddling_tau, straddling_units] = llmcc::DetectSemanticUnits(
      straddling, {}, two_lines,
      {.kind = llmcc::TauRule::Kind::kAbsolute, .value = 1.0});
  ExpectEq(straddling_tau, 1.0, "straddling tau");
  ExpectEq(straddling_units.size(), std::size_t{1},
           "straddling token snaps to its starting line");

  const llmcc::Analysis empty = llmcc::Analyze({}, {});
  ExpectEq(empty.llm_cc, 0.0, "empty score");
  ExpectEq(empty.total_branch, std::uint64_t{0}, "empty branch total");
  ExpectEq(empty.total_comp_level, std::uint64_t{0}, "empty level total");
  ExpectEq(empty.metrics, llmcc::Metrics{}, "empty metrics");

  const auto plain_tokens = ByteTokens({std::nullopt, 0.0, 0.0});
  const llmcc::Analysis root_only =
      llmcc::Analyze(plain_tokens, {}, {},
                     {.kind = llmcc::TauRule::Kind::kAbsolute, .value = 1.0},
                     0.8, std::vector<llmcc::SourceRange>{{0, 3}});
  ExpectEq(root_only.units.size(), std::size_t{0},
           "undivided input has no duplicate root child");
  Expect(std::abs(root_only.llm_cc - 0.2) < 1e-12,
         "undivided input scores one minus alpha");

  const std::vector<llmcc::StructuralEvent> termination = {
      {.scope_start = 0, .byte_offset = 2, .depth = 0}};
  const llmcc::Analysis structural =
      llmcc::Analyze(plain_tokens, termination, {},
                     {.kind = llmcc::TauRule::Kind::kAbsolute, .value = 1.0},
                     0.8, std::vector<llmcc::SourceRange>{{0, 3}},
                     llmcc::HierarchyMode::kStructural);
  const llmcc::Analysis reference =
      llmcc::Analyze(plain_tokens, termination, {},
                     {.kind = llmcc::TauRule::Kind::kAbsolute, .value = 1.0},
                     0.8, std::vector<llmcc::SourceRange>{{0, 3}},
                     llmcc::HierarchyMode::kReference);
  Expect(structural.llm_cc > reference.llm_cc,
         "reference mode uses entropy-led boundaries");
  Expect(std::abs(reference.llm_cc - 0.2) < 1e-12,
         "reference root-only behavior matches pinned implementation");

  // Golden topology for the authors' c38a26af revision: the text before the
  // first marker stays in the implicit root and each marker starts a block.
  const auto reference_tokens =
      ByteTokens({std::nullopt, 0.0, 2.0, 0.0, 2.0, 0.0});
  const std::vector<std::size_t> reference_lines = {0, 1, 2, 3, 4, 5};
  const std::vector<llmcc::StructuralEvent> reference_scopes = {
      {.scope_start = 0, .byte_offset = 6, .depth = 0},
      {.scope_start = 4, .byte_offset = 6, .depth = 1},
  };
  const llmcc::Analysis reference_golden =
      llmcc::Analyze(reference_tokens, reference_scopes, reference_lines,
                     {.kind = llmcc::TauRule::Kind::kAbsolute, .value = 1.0},
                     0.8, std::vector<llmcc::SourceRange>{{0, 6}},
                     llmcc::HierarchyMode::kReference);
  ExpectEq(reference_golden.units.size(), std::size_t{1},
           "reference marker blocks share the implicit root");
  ExpectEq(reference_golden.units.front().children.size(), std::size_t{1},
           "reference control scope nests the later marker block");
  ExpectEq(reference_golden.total_branch, std::uint64_t{2},
           "reference golden branch total");
  ExpectEq(reference_golden.total_comp_level, std::uint64_t{6},
           "reference golden level total");
  Expect(std::abs(reference_golden.llm_cc - 2.8) < 1e-12,
         "reference golden raw score");

  const auto whitespace_tokens = ByteTokens({std::nullopt, 0.0, 0.0, 0.0, 0.0});
  const llmcc::Analysis with_trailing_whitespace =
      llmcc::Analyze(whitespace_tokens, {}, {},
                     {.kind = llmcc::TauRule::Kind::kAbsolute, .value = 1.0},
                     0.8, std::vector<llmcc::SourceRange>{{0, 3}});
  ExpectEq(with_trailing_whitespace.llm_cc, root_only.llm_cc,
           "trailing whitespace does not change raw LM-CC");

  const auto metric_tokens = ByteTokens({std::nullopt, 0.5, 1.0});
  const llmcc::Analysis metric_analysis = llmcc::Analyze(
      metric_tokens, {}, {},
      {.kind = llmcc::TauRule::Kind::kAbsolute, .value = 1.0}, 0.5);
  ExpectEq(metric_analysis.metrics.token_count, std::uint64_t{2},
           "metric scored-token count");
  ExpectEq(metric_analysis.metrics.high_entropy_tokens, std::uint64_t{1},
           "metric high-entropy count includes equality");
  ExpectEq(metric_analysis.metrics.entropy_sum, 1.5, "metric entropy sum");
  ExpectEq(metric_analysis.metrics.lmcc, metric_analysis.llm_cc,
           "metric lmcc duplicates score");
  Expect(std::abs(metric_analysis.metrics.lmcc_per_token -
                  (metric_analysis.llm_cc / 2.0)) < 1e-12,
         "metric lmcc per token");
  Expect(std::abs(metric_analysis.metrics.density - 0.5) < 1e-12,
         "metric density");
  ExpectEq(metric_analysis.metrics.mean_entropy, 0.75,
           "metric mean entropy uses scored tokens");
  return 0;
}
