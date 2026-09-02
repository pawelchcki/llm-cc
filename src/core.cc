#include "src/core.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <set>
#include <utility>

namespace llmcc {
namespace {

using Range = std::pair<std::size_t, std::size_t>;

std::vector<Range> PartitionAtShallowest(std::span<const SemanticUnit> units,
                                         std::size_t first, std::size_t end) {
  if (first >= end) {
    return {};
  }
  std::size_t shallowest = std::numeric_limits<std::size_t>::max();
  for (std::size_t i = first; i < end; ++i) {
    shallowest = std::min(shallowest, units[i].nesting_depth);
  }
  std::vector<std::size_t> starts;
  for (std::size_t i = first; i < end; ++i) {
    if (units[i].nesting_depth == shallowest) {
      starts.push_back(i);
    }
  }
  std::vector<Range> ranges;
  if (starts.front() > first) {
    ranges.emplace_back(first, starts.front());
  }
  for (std::size_t i = 0; i < starts.size(); ++i) {
    ranges.emplace_back(starts[i], i + 1 < starts.size() ? starts[i + 1] : end);
  }
  return ranges;
}

std::pair<std::uint64_t, std::uint64_t> Totals(std::span<const Unit> units) {
  std::uint64_t branches = 0;
  std::uint64_t levels = 0;
  for (const Unit& unit : units) {
    const auto [child_branches, child_levels] = Totals(unit.children);
    branches += unit.branching + child_branches;
    levels += unit.level + child_levels;
  }
  return {branches, levels};
}

void ValidateInputs(std::span<const Token> tokens,
                    std::span<const StructuralEvent> events,
                    std::span<const std::size_t> line_starts) {
  std::size_t previous_end = 0;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const Token& token = tokens[i];
    if (token.start_byte >= token.end_byte ||
        (i > 0 && token.start_byte < previous_end)) {
      throw AnalysisError(
          "token byte ranges must be ordered, non-overlapping, and non-empty");
    }
    previous_end = token.end_byte;
  }
  const std::size_t source_end = tokens.empty() ? 0 : tokens.back().end_byte;
  for (const StructuralEvent& event : events) {
    if (event.scope_start >= event.byte_offset ||
        event.byte_offset > source_end) {
      throw AnalysisError("structural event range is invalid");
    }
  }
  if (!line_starts.empty() &&
      (line_starts.front() != 0 || !std::ranges::is_sorted(line_starts) ||
       std::ranges::adjacent_find(line_starts) != line_starts.end())) {
    throw AnalysisError("line starts must begin at zero and be ascending");
  }
}

}  // namespace

std::optional<double> Percentile(std::span<const double> values,
                                 double percentile) {
  if (!std::isfinite(percentile) || percentile < 0.0 || percentile > 100.0) {
    throw AnalysisError("tau percentile must be finite and between 0 and 100");
  }
  if (values.empty()) {
    return std::nullopt;
  }
  std::vector<double> sorted(values.begin(), values.end());
  if (std::ranges::any_of(sorted,
                          [](double value) { return !std::isfinite(value); })) {
    throw AnalysisError("token entropy must be finite and non-negative");
  }
  std::ranges::sort(sorted);
  const double rank =
      percentile / 100.0 * static_cast<double>(sorted.size() - 1);
  const auto lower = static_cast<std::size_t>(std::floor(rank));
  const auto upper = static_cast<std::size_t>(std::ceil(rank));
  return sorted[lower] + ((sorted[upper] - sorted[lower]) *
                          (rank - static_cast<double>(lower)));
}

std::size_t TokenIndexAt(std::span<const Token> tokens, std::size_t byte) {
  const auto iterator = std::ranges::partition_point(
      tokens, [&](const Token& token) { return token.end_byte < byte; });
  std::size_t index =
      static_cast<std::size_t>(std::distance(tokens.begin(), iterator));
  if (index < tokens.size() && tokens[index].start_byte < byte) {
    ++index;
  }
  return index;
}

std::pair<double, std::vector<SemanticUnit>> DetectSemanticUnits(
    std::span<const Token> tokens,
    std::span<const StructuralEvent> structural_events,
    std::span<const std::size_t> line_starts, TauRule tau_rule) {
  ValidateInputs(tokens, structural_events, line_starts);
  std::vector<double> entropy_values;
  for (const Token& token : tokens) {
    if (token.entropy.has_value()) {
      if (!std::isfinite(*token.entropy) || *token.entropy < 0.0) {
        throw AnalysisError("token entropy must be finite and non-negative");
      }
      entropy_values.push_back(*token.entropy);
    }
  }
  double tau = tau_rule.value;
  switch (tau_rule.kind) {
    case TauRule::Kind::kAbsolute:
      if (!std::isfinite(tau) || tau < 0.0) {
        throw AnalysisError("absolute tau must be finite and non-negative");
      }
      break;
    case TauRule::Kind::kPercentile:
      tau = Percentile(entropy_values, tau_rule.value).value_or(0.0);
      break;
  }
  if (tokens.empty()) {
    return {tau, {}};
  }

  std::vector<std::size_t> first_token_on_line(tokens.size());
  if (line_starts.empty()) {
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      first_token_on_line[i] = i;
    }
  } else {
    std::size_t line = 0;
    std::size_t first_token = 0;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      while (line + 1 < line_starts.size() &&
             line_starts[line + 1] <= tokens[i].start_byte) {
        ++line;
      }
      while (first_token < i &&
             tokens[first_token].start_byte < line_starts[line]) {
        ++first_token;
      }
      first_token_on_line[i] = first_token;
    }
  }

  std::set<std::size_t> boundaries = {0, tokens.size()};
  for (std::size_t i = 1; i < tokens.size(); ++i) {
    if (tokens[i].entropy.has_value() && *tokens[i].entropy >= tau) {
      boundaries.insert(first_token_on_line[i]);
    }
  }
  // Structural boundaries deliberately remain token-granular, an extension
  // over the reference implementation's entropy-only line snapping.
  for (const StructuralEvent& event : structural_events) {
    boundaries.insert(TokenIndexAt(tokens, event.byte_offset));
  }

  std::vector<std::size_t> indices(boundaries.begin(), boundaries.end());
  std::vector<SemanticUnit> units;
  for (std::size_t i = 1; i < indices.size(); ++i) {
    const std::size_t start_index = indices[i - 1];
    const std::size_t end_index = indices[i];
    if (start_index == end_index) {
      continue;
    }
    const std::size_t start_byte = tokens[start_index].start_byte;
    std::size_t depth = 0;
    for (const StructuralEvent& event : structural_events) {
      if (event.scope_start <= start_byte && start_byte < event.byte_offset) {
        depth = std::max(depth, event.depth);
      }
    }
    units.push_back({.start_byte = start_byte,
                     .end_byte = tokens[end_index - 1].end_byte,
                     .nesting_depth = depth});
  }
  return {tau, std::move(units)};
}

std::vector<Unit> BuildHierarchy(std::span<const SemanticUnit> semantic_units) {
  if (semantic_units.empty()) {
    return {};
  }
  struct ArenaUnit {
    std::size_t first;
    std::size_t end;
    std::uint64_t level;
    std::vector<std::size_t> children;
  };
  std::vector<ArenaUnit> arena;
  for (const auto [first, end] :
       PartitionAtShallowest(semantic_units, 0, semantic_units.size())) {
    arena.push_back({.first = first, .end = end, .level = 2, .children = {}});
  }
  std::vector<std::size_t> roots(arena.size());
  for (std::size_t i = 0; i < roots.size(); ++i) {
    roots[i] = i;
  }
  std::deque<std::size_t> queue(roots.begin(), roots.end());
  while (!queue.empty()) {
    const std::size_t parent = queue.front();
    queue.pop_front();
    const std::size_t first = arena[parent].first;
    const std::size_t end = arena[parent].end;
    if (end - first <= 1) {
      continue;
    }
    const std::uint64_t level = arena[parent].level + 1;
    for (const auto [child_first, child_end] :
         PartitionAtShallowest(semantic_units, first + 1, end)) {
      const std::size_t child = arena.size();
      arena.push_back({.first = child_first,
                       .end = child_end,
                       .level = level,
                       .children = {}});
      arena[parent].children.push_back(child);
      queue.push_back(child);
    }
  }

  std::function<Unit(std::size_t)> materialize;
  materialize = [&](std::size_t index) -> Unit {
    const ArenaUnit& node = arena[index];
    std::vector<Unit> children;
    children.reserve(node.children.size());
    for (std::size_t child : node.children) {
      children.push_back(materialize(child));
    }
    return {.start_byte = semantic_units[node.first].start_byte,
            .end_byte = semantic_units[node.end - 1].end_byte,
            .level = node.level,
            .branching = static_cast<std::uint64_t>(children.size()),
            .children = std::move(children)};
  };
  std::vector<Unit> units;
  units.reserve(roots.size());
  for (std::size_t root : roots) {
    units.push_back(materialize(root));
  }
  return units;
}

Analysis Analyze(std::span<const Token> tokens,
                 std::span<const StructuralEvent> structural_events,
                 std::span<const std::size_t> line_starts, TauRule tau_rule,
                 double alpha) {
  if (!std::isfinite(alpha) || alpha < 0.0 || alpha > 1.0) {
    throw AnalysisError("alpha must be finite and between 0 and 1");
  }
  auto [tau, semantic_units] =
      DetectSemanticUnits(tokens, structural_events, line_starts, tau_rule);
  std::vector<Unit> units = BuildHierarchy(semantic_units);
  auto [branches, levels] = Totals(units);
  if (!units.empty()) {
    branches += units.size();
    levels += 1;
  }
  const double lmcc = (alpha * static_cast<double>(branches)) +
                      ((1.0 - alpha) * static_cast<double>(levels));
  Metrics metrics;
  metrics.lmcc = lmcc;
  for (const Token& token : tokens) {
    if (token.entropy.has_value()) {
      ++metrics.token_count;
      metrics.entropy_sum += *token.entropy;
      if (*token.entropy >= tau) {
        ++metrics.high_entropy_tokens;
      }
    }
  }
  if (metrics.token_count != 0) {
    const double token_count = static_cast<double>(metrics.token_count);
    metrics.lmcc_per_token = metrics.lmcc / token_count;
    metrics.density =
        static_cast<double>(metrics.high_entropy_tokens) / token_count;
    metrics.mean_entropy = metrics.entropy_sum / token_count;
  }
  return {.llm_cc = lmcc,
          .total_branch = branches,
          .total_comp_level = levels,
          .alpha = alpha,
          .tau = tau,
          .metrics = metrics,
          .tau_rule = tau_rule,
          .units = std::move(units)};
}

}  // namespace llmcc
