#include "src/analyze.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <ranges>
#include <stdexcept>

#include "src/lang.h"

namespace llmcc {

namespace {

std::size_t FirstOverlappingToken(std::span<const Token> tokens,
                                  std::size_t byte) {
  const auto iterator = std::ranges::partition_point(
      tokens, [&](const Token& token) { return token.end_byte <= byte; });
  return static_cast<std::size_t>(std::distance(tokens.begin(), iterator));
}

}  // namespace

ProjectAnalyzer::ProjectAnalyzer(ProjectAnalysisOptions options,
                                 ProviderFactory factory)
    : options_(std::move(options)), factory_(std::move(factory)) {}

FileAnalysisResult ProjectAnalyzer::AnalyzeFile(const DiscoveredSource& source,
                                                std::string_view contents) {
  auto [preprocessed, offset_map] = StripComments(contents, source.language);
  assert(std::ranges::count(preprocessed, '\n') ==
         std::ranges::count(contents, '\n'));
  const auto events = StructuralEvents(preprocessed, source.language);
  EntropyCacheLookup cached;
  if (options_.cache && source.repository.has_value()) {
    cached = ReadEntropyCache(*source.repository, preprocessed, options_.model);
  }
  std::vector<EntropyRecord> records;
  if (cached.hit) {
    records = std::move(cached.records);
  } else {
    if (!provider_) {
      if (initialization_error_.has_value()) {
        throw ScorerInitializationError(*initialization_error_);
      }
      try {
        provider_ = factory_();
      } catch (const std::exception& error) {
        initialization_error_ = error.what();
        throw ScorerInitializationError(*initialization_error_);
      }
      if (!provider_) {
        initialization_error_ = "entropy provider factory returned null";
        throw ScorerInitializationError(*initialization_error_);
      }
    }
    records = provider_->Score(preprocessed);
    if (options_.cache && source.repository.has_value()) {
      try {
        WriteEntropyCache(*source.repository, preprocessed, options_.model,
                          records);
      } catch (const std::exception& error) {
        // Repository-local caching is advisory and must not lose an analysis.
        static_cast<void>(error);
      }
    }
  }
  const auto tokens = AlignTokens(preprocessed, records);
  const auto line_starts = LineStarts(preprocessed);
  Analysis analysis = llmcc::Analyze(tokens, events, line_starts,
                                     options_.tau_rule, options_.alpha);
  const double file_tau = analysis.tau;

  const auto line_at = [&](std::size_t byte) {
    return static_cast<std::size_t>(
        std::ranges::upper_bound(line_starts, byte) - line_starts.begin());
  };
  std::vector<FunctionScore> functions;
  for (const FunctionSpan& function :
       Functions(preprocessed, source.language)) {
    const std::size_t first =
        FirstOverlappingToken(tokens, function.start_byte);
    const std::size_t end = TokenIndexAt(tokens, function.end_byte);
    if (first >= end) {
      continue;
    }
    const std::span<const Token> slice(tokens.data() + first, end - first);
    std::vector<StructuralEvent> function_events;
    for (const StructuralEvent& event : events) {
      if (event.scope_start >= slice.front().start_byte &&
          event.byte_offset <= slice.back().end_byte) {
        function_events.push_back(event);
      }
    }
    std::vector<std::size_t> function_line_starts = {0};
    for (std::size_t line_start : line_starts) {
      if (line_start > slice.front().start_byte &&
          line_start <= slice.back().start_byte) {
        function_line_starts.push_back(line_start);
      }
    }
    Analysis function_analysis = llmcc::Analyze(
        slice, function_events, function_line_starts,
        {.kind = TauRule::Kind::kAbsolute, .value = file_tau}, options_.alpha);
    functions.push_back(
        {.name = function.name,
         .start_line = line_at(function.start_byte),
         .end_line =
             line_at(function.end_byte == 0 ? 0 : function.end_byte - 1),
         .metrics = function_analysis.metrics});
  }

  struct LineMetrics {
    std::size_t line;
    double max_entropy = 0.0;
    double entropy_sum = 0.0;
    std::uint64_t token_count = 0;
    std::uint64_t high_tokens = 0;
  };
  std::vector<LineMetrics> lines;
  if (options_.hotspots != 0) {
    std::size_t line = 0;
    for (const Token& token : tokens) {
      while (line + 1 < line_starts.size() &&
             line_starts[line + 1] <= token.start_byte) {
        ++line;
      }
      if (!token.entropy.has_value()) {
        continue;
      }
      if (lines.empty() || lines.back().line != line + 1) {
        lines.push_back({.line = line + 1});
      }
      LineMetrics& metrics = lines.back();
      metrics.max_entropy = std::max(metrics.max_entropy, *token.entropy);
      metrics.entropy_sum += *token.entropy;
      ++metrics.token_count;
      if (*token.entropy >= file_tau) {
        ++metrics.high_tokens;
      }
    }
  }
  std::vector<Hotspot> hotspots;
  hotspots.reserve(lines.size());
  for (const LineMetrics& line : lines) {
    hotspots.push_back({.line = line.line,
                        .max_entropy = line.max_entropy,
                        .mean_entropy = line.entropy_sum /
                                        static_cast<double>(line.token_count),
                        .high_tokens = line.high_tokens});
  }
  std::ranges::sort(hotspots, [](const Hotspot& left, const Hotspot& right) {
    if (left.max_entropy != right.max_entropy) {
      return left.max_entropy > right.max_entropy;
    }
    if (left.mean_entropy != right.mean_entropy) {
      return left.mean_entropy > right.mean_entropy;
    }
    return left.line < right.line;
  });
  if (hotspots.size() > options_.hotspots) {
    hotspots.resize(options_.hotspots);
  }

  MapAnalysisOffsets(analysis, offset_map);
  return {.analysis = std::move(analysis),
          .entropy_cache_hit = cached.hit,
          .functions = std::move(functions),
          .hotspots = std::move(hotspots)};
}

bool ProjectAnalyzer::ScorerLoaded() const { return provider_ != nullptr; }

}  // namespace llmcc
