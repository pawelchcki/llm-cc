#include "src/analyze.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <ranges>
#include <stdexcept>

#include "src/backend.h"
#include "src/lang.h"

namespace llmcc {

namespace {

std::size_t FirstOverlappingToken(std::span<const Token> tokens,
                                  std::size_t byte) {
  const auto iterator = std::ranges::partition_point(
      tokens, [&](const Token& token) { return token.end_byte <= byte; });
  return static_cast<std::size_t>(std::distance(tokens.begin(), iterator));
}

std::vector<StructuralEvent> FunctionEvents(
    std::span<const StructuralEvent> events, const FunctionSpan& function) {
  std::vector<StructuralEvent> result;
  const auto first = std::ranges::lower_bound(events, function.start_byte, {},
                                              &StructuralEvent::scope_start);
  for (auto event = first;
       event != events.end() && event->scope_start < function.end_byte;
       ++event) {
    if (event->byte_offset <= function.end_byte) {
      result.push_back(*event);
    }
  }
  return result;
}

std::vector<SourceRange> FunctionRanges(std::span<const SourceRange> ranges,
                                        const FunctionSpan& function) {
  std::vector<SourceRange> result;
  const auto first =
      std::ranges::partition_point(ranges, [&](const SourceRange& range) {
        return range.end_byte <= function.start_byte;
      });
  for (auto range = first;
       range != ranges.end() && range->start_byte < function.end_byte;
       ++range) {
    const std::size_t start = std::max(range->start_byte, function.start_byte);
    const std::size_t end = std::min(range->end_byte, function.end_byte);
    if (start < end) {
      result.push_back({start, end});
    }
  }
  return result;
}

std::vector<FunctionScore> ScoreFunctions(
    const PreparedSource& prepared, std::span<const Token> tokens,
    double file_tau, const ProjectAnalysisOptions& options) {
  const auto& events = prepared.structural_events;
  const auto& line_starts = prepared.line_starts;
  const auto line_at = [&](std::size_t byte) {
    return static_cast<std::size_t>(
        std::ranges::upper_bound(line_starts, byte) - line_starts.begin());
  };
  std::vector<FunctionScore> functions;
  for (const FunctionSpan& function : prepared.functions) {
    const std::size_t first =
        FirstOverlappingToken(tokens, function.start_byte);
    const std::size_t end = TokenIndexAt(tokens, function.end_byte);
    if (first >= end) {
      continue;
    }
    const std::span<const Token> function_span =
        tokens.subspan(first, end - first);
    std::vector<Token> function_tokens(function_span.begin(),
                                       function_span.end());
    function_tokens.front().start_byte =
        std::max(function_tokens.front().start_byte, function.start_byte);
    const auto function_events = FunctionEvents(events, function);
    std::vector<std::size_t> function_line_starts = {0};
    const auto first_line = std::ranges::upper_bound(
        line_starts, function_tokens.front().start_byte);
    const auto last_line = std::ranges::upper_bound(
        line_starts, function_tokens.back().start_byte);
    function_line_starts.insert(function_line_starts.end(), first_line,
                                last_line);
    const auto function_meaningful =
        FunctionRanges(prepared.meaningful_ranges, function);
    Analysis function_analysis = llmcc::Analyze(
        function_tokens, function_events, function_line_starts,
        {.kind = TauRule::Kind::kAbsolute, .value = file_tau}, options.alpha,
        function_meaningful, options.hierarchy_mode);
    functions.push_back(
        {.name = function.name,
         .start_line = line_at(function.start_byte),
         .end_line =
             line_at(function.end_byte == 0 ? 0 : function.end_byte - 1),
         .metrics = function_analysis.metrics});
  }
  return functions;
}

std::vector<Hotspot> FindHotspots(std::span<const Token> tokens,
                                  std::span<const std::size_t> line_starts,
                                  double file_tau, std::size_t limit) {
  if (limit == 0) {
    return {};
  }
  struct LineMetrics {
    std::size_t line;
    double max_entropy = 0.0;
    double entropy_sum = 0.0;
    std::uint64_t token_count = 0;
    std::uint64_t high_tokens = 0;
  };
  std::vector<LineMetrics> lines;
  std::size_t line = 0;
  for (const Token& token : tokens) {
    while (line + 1 < line_starts.size() &&
           line_starts[line + 1] <= token.start_byte) {
      ++line;
    }
    if (!token.entropy.has_value()) {
      continue;
    }
    const double entropy = *token.entropy;
    if (lines.empty() || lines.back().line != line + 1) {
      lines.push_back({.line = line + 1});
    }
    LineMetrics& metrics = lines.back();
    metrics.max_entropy = std::max(metrics.max_entropy, entropy);
    metrics.entropy_sum += entropy;
    ++metrics.token_count;
    if (entropy >= file_tau) {
      ++metrics.high_tokens;
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
  if (hotspots.size() > limit) {
    hotspots.resize(limit);
  }
  return hotspots;
}

}  // namespace

ProjectAnalyzer::ProjectAnalyzer(ProjectAnalysisOptions options,
                                 ProviderFactory factory)
    : options_(std::move(options)), factory_(std::move(factory)) {}

EntropyProvider& ProjectAnalyzer::Provider() {
  if (provider_) {
    return *provider_;
  }
  if (initialization_error_.has_value()) {
    throw ScorerInitializationError(*initialization_error_);
  }
  try {
    provider_ = factory_();
  } catch (const GpuRecoverableError&) {
    throw;
  } catch (const std::exception& error) {
    initialization_error_ = error.what();
    throw ScorerInitializationError(*initialization_error_);
  }
  if (!provider_) {
    initialization_error_ = "entropy provider factory returned null";
    throw ScorerInitializationError(*initialization_error_);
  }
  return *provider_;
}

EntropyCacheLookup ProjectAnalyzer::ReadRecords(std::string_view source) {
  if (options_.cache) {
    auto cached = ReadEntropyCache(source, options_.model);
    if (cached.hit) {
      return cached;
    }
  }
  auto records = Provider().Score(source);
  if (options_.cache) {
    try {
      WriteEntropyCache(source, options_.model, records);
    } catch (const std::exception&) {
      // Entropy caching is advisory and must not lose an analysis.
    }
  }
  return {.hit = false, .records = std::move(records)};
}

FileAnalysisResult ProjectAnalyzer::AnalyzeFile(const DiscoveredSource& source,
                                                std::string_view contents) {
  PreparedSource prepared = PrepareSource(contents, source.language);
  const std::string& preprocessed = prepared.cleaned;
  assert(std::ranges::count(preprocessed, '\n') ==
         std::ranges::count(contents, '\n'));
  if (prepared.meaningful_ranges.empty()) {
    Analysis analysis =
        llmcc::Analyze({}, {}, {}, options_.tau_rule, options_.alpha, {},
                       options_.hierarchy_mode);
    MapAnalysisOffsets(analysis, prepared.original_offsets);
    return {.analysis = std::move(analysis), .entropy_cache_hit = false};
  }
  const auto cached = ReadRecords(preprocessed);
  const auto tokens = AlignTokens(preprocessed, cached.records);
  Analysis analysis =
      llmcc::Analyze(tokens, prepared.structural_events, prepared.line_starts,
                     options_.tau_rule, options_.alpha,
                     prepared.meaningful_ranges, options_.hierarchy_mode);
  auto functions = ScoreFunctions(prepared, tokens, analysis.tau, options_);
  auto hotspots = FindHotspots(tokens, prepared.line_starts, analysis.tau,
                               options_.hotspots);

  MapAnalysisOffsets(analysis, prepared.original_offsets);
  return {.analysis = std::move(analysis),
          .entropy_cache_hit = cached.hit,
          .functions = std::move(functions),
          .hotspots = std::move(hotspots)};
}

bool ProjectAnalyzer::ScorerLoaded() const { return provider_ != nullptr; }

}  // namespace llmcc
