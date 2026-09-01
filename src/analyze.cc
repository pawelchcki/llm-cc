#include "src/analyze.h"

#include <stdexcept>

#include "src/lang.h"

namespace llmcc {

ProjectAnalyzer::ProjectAnalyzer(ProjectAnalysisOptions options,
                                 ProviderFactory factory)
    : options_(std::move(options)), factory_(std::move(factory)) {}

FileAnalysisResult ProjectAnalyzer::AnalyzeFile(const DiscoveredSource& source,
                                                std::string_view contents) {
  auto [preprocessed, offset_map] = StripComments(contents, source.language);
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
  Analysis analysis =
      llmcc::Analyze(tokens, events, options_.tau_percentile, options_.alpha);
  MapAnalysisOffsets(analysis, offset_map);
  return {.analysis = std::move(analysis), .entropy_cache_hit = cached.hit};
}

bool ProjectAnalyzer::ScorerLoaded() const { return provider_ != nullptr; }

}  // namespace llmcc
