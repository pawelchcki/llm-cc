#include "src/core.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <utility>

namespace llmcc {
namespace {

std::pair<std::uint64_t, std::uint64_t> Totals(std::span<const Unit> units) {
  std::uint64_t branches = 0;
  std::uint64_t levels = 0;
  std::vector<const Unit*> pending;
  for (const Unit& unit : units) {
    pending.push_back(&unit);
  }
  while (!pending.empty()) {
    const Unit& unit = *pending.back();
    pending.pop_back();
    branches += unit.branching;
    levels += unit.level;
    for (const Unit& child : unit.children) {
      pending.push_back(&child);
    }
  }
  return {branches, levels};
}

std::vector<Unit> BuildHierarchyImpl(
    std::span<const SemanticUnit> semantic_units, bool suppress_single_root);

std::size_t FirstOverlappingToken(std::span<const Token> tokens,
                                  std::size_t byte) {
  const auto iterator = std::ranges::partition_point(
      tokens, [&](const Token& token) { return token.end_byte <= byte; });
  return static_cast<std::size_t>(std::distance(tokens.begin(), iterator));
}

void ValidateInputs(std::span<const Token> tokens,
                    std::span<const StructuralEvent> events,
                    std::span<const std::size_t> line_starts,
                    std::span<const SourceRange> meaningful_ranges) {
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
  std::size_t previous_range_end = 0;
  for (const SourceRange& range : meaningful_ranges) {
    if (range.start_byte >= range.end_byte || range.end_byte > source_end ||
        range.start_byte < previous_range_end) {
      throw AnalysisError(
          "meaningful source ranges must be ordered and non-overlapping");
    }
    previous_range_end = range.end_byte;
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
    std::span<const std::size_t> line_starts, TauRule tau_rule,
    std::span<const SourceRange> meaningful_ranges,
    HierarchyMode hierarchy_mode) {
  ValidateInputs(tokens, structural_events, line_starts, meaningful_ranges);
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

  const bool source_layout_supplied = !meaningful_ranges.empty();
  const std::size_t meaningful_start =
      source_layout_supplied ? meaningful_ranges.front().start_byte
                             : tokens.front().start_byte;
  const std::size_t meaningful_end = source_layout_supplied
                                         ? meaningful_ranges.back().end_byte
                                         : tokens.back().end_byte;

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

  const std::size_t source_start =
      FirstOverlappingToken(tokens, meaningful_start);
  const std::size_t source_end = TokenIndexAt(tokens, meaningful_end);
  std::set<std::size_t> boundaries = {source_start, source_end};
  bool marker_at_source_start = false;
  for (std::size_t i = std::max<std::size_t>(1, source_start); i < source_end;
       ++i) {
    if (tokens[i].entropy.has_value() && *tokens[i].entropy >= tau) {
      const std::size_t boundary =
          std::max(first_token_on_line[i], source_start);
      if (boundary == source_start) {
        marker_at_source_start = true;
      } else if (boundary > source_start && boundary < source_end) {
        boundaries.insert(boundary);
      }
    }
  }
  // Structural boundaries deliberately remain token-granular, an extension
  // over the reference implementation's entropy-only line snapping.
  if (hierarchy_mode == HierarchyMode::kStructural) {
    for (const StructuralEvent& event : structural_events) {
      const std::size_t boundary = TokenIndexAt(tokens, event.byte_offset);
      if (boundary > source_start && boundary < source_end) {
        boundaries.insert(boundary);
      }
    }
  }

  std::vector<std::size_t> indices(boundaries.begin(), boundaries.end());
  std::vector<SemanticUnit> units;
  std::vector<StructuralEvent> events(structural_events.begin(),
                                      structural_events.end());
  std::ranges::sort(events, {}, &StructuralEvent::scope_start);
  using EndDepth = std::pair<std::size_t, std::size_t>;
  std::priority_queue<EndDepth, std::vector<EndDepth>, std::greater<>> ending;
  std::multiset<std::size_t> active_depths;
  std::size_t event_index = 0;
  std::size_t range_index = 0;
  // The authors' reference tree keeps the pre-marker text in the implicit
  // root and creates one block for each entropy marker. Structural mode is a
  // true partition, so it retains the interval before the first boundary too.
  const std::size_t first_interval =
      hierarchy_mode == HierarchyMode::kReference && !marker_at_source_start
          ? 1
          : 0;
  for (std::size_t i = first_interval; i + 1 < indices.size(); ++i) {
    const std::size_t start_index = indices[i];
    const std::size_t end_index = indices[i + 1];
    if (start_index == end_index) {
      continue;
    }
    const std::size_t interval_start = tokens[start_index].start_byte;
    const std::size_t interval_end = tokens[end_index - 1].end_byte;
    while (range_index < meaningful_ranges.size() &&
           meaningful_ranges[range_index].end_byte <= interval_start) {
      ++range_index;
    }
    std::size_t start_byte = interval_start;
    if (source_layout_supplied) {
      if (range_index >= meaningful_ranges.size() ||
          meaningful_ranges[range_index].start_byte >= interval_end) {
        continue;
      }
      start_byte =
          std::max(interval_start, meaningful_ranges[range_index].start_byte);
    }
    while (event_index < events.size() &&
           events[event_index].scope_start <= start_byte) {
      ending.emplace(events[event_index].byte_offset,
                     events[event_index].depth);
      active_depths.insert(events[event_index].depth);
      ++event_index;
    }
    while (!ending.empty() && ending.top().first <= start_byte) {
      const auto found = active_depths.find(ending.top().second);
      if (found != active_depths.end()) {
        active_depths.erase(found);
      }
      ending.pop();
    }
    const std::size_t depth =
        active_depths.empty() ? 0 : *active_depths.rbegin();
    units.push_back({.start_byte = start_byte,
                     .end_byte = std::min(interval_end, meaningful_end),
                     .nesting_depth = depth});
  }
  return {tau, std::move(units)};
}

std::vector<Unit> BuildHierarchy(std::span<const SemanticUnit> semantic_units) {
  return BuildHierarchyImpl(semantic_units, true);
}

namespace {

std::vector<Unit> BuildHierarchyImpl(
    std::span<const SemanticUnit> semantic_units, bool suppress_single_root) {
  // A single effective semantic interval is represented by the implicit root,
  // not by a duplicate whole-input child.
  if (semantic_units.empty() ||
      (suppress_single_root && semantic_units.size() == 1)) {
    return {};
  }
  struct ArenaUnit {
    std::size_t start_byte;
    std::size_t end_byte;
    std::size_t depth;
    std::uint64_t level;
    std::vector<std::size_t> children;
  };
  std::vector<ArenaUnit> arena;
  arena.reserve(semantic_units.size());
  std::vector<std::size_t> roots;
  std::vector<std::size_t> stack;
  for (const SemanticUnit& semantic : semantic_units) {
    while (!stack.empty() &&
           arena[stack.back()].depth >= semantic.nesting_depth) {
      stack.pop_back();
    }
    const std::uint64_t level =
        stack.empty() ? 2 : arena[stack.back()].level + 1;
    const std::size_t index = arena.size();
    arena.push_back({.start_byte = semantic.start_byte,
                     .end_byte = semantic.end_byte,
                     .depth = semantic.nesting_depth,
                     .level = level,
                     .children = {}});
    if (stack.empty()) {
      roots.push_back(index);
    } else {
      arena[stack.back()].children.push_back(index);
    }
    stack.push_back(index);
  }
  for (std::size_t index = arena.size(); index-- > 0;) {
    for (std::size_t child : arena[index].children) {
      arena[index].end_byte =
          std::max(arena[index].end_byte, arena[child].end_byte);
    }
  }

  std::vector<Unit> materialized(arena.size());
  for (std::size_t index = arena.size(); index-- > 0;) {
    const ArenaUnit& node = arena[index];
    std::vector<Unit> children;
    children.reserve(node.children.size());
    for (std::size_t child : node.children) {
      children.push_back(std::move(materialized[child]));
    }
    materialized[index] = {
        .start_byte = node.start_byte,
        .end_byte = node.end_byte,
        .level = node.level,
        .branching = static_cast<std::uint64_t>(children.size()),
        .children = std::move(children),
    };
  }
  std::vector<Unit> units;
  units.reserve(roots.size());
  for (std::size_t root : roots) {
    units.push_back(std::move(materialized[root]));
  }
  return units;
}

}  // namespace

Analysis Analyze(std::span<const Token> tokens,
                 std::span<const StructuralEvent> structural_events,
                 std::span<const std::size_t> line_starts, TauRule tau_rule,
                 double alpha, std::span<const SourceRange> meaningful_ranges,
                 HierarchyMode hierarchy_mode) {
  if (!std::isfinite(alpha) || alpha < 0.0 || alpha > 1.0) {
    throw AnalysisError("alpha must be finite and between 0 and 1");
  }
  auto [tau, semantic_units] =
      DetectSemanticUnits(tokens, structural_events, line_starts, tau_rule,
                          meaningful_ranges, hierarchy_mode);
  std::vector<Unit> units = BuildHierarchyImpl(
      semantic_units, hierarchy_mode == HierarchyMode::kStructural);
  auto [branches, levels] = Totals(units);
  const bool has_meaningful_source =
      !meaningful_ranges.empty() || !tokens.empty();
  if (has_meaningful_source) {
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
          .hierarchy_mode = hierarchy_mode,
          .units = std::move(units)};
}

}  // namespace llmcc
