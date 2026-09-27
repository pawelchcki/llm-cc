#ifndef LLM_CC_ANALYZE_H_
#define LLM_CC_ANALYZE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "src/entropy_cache.h"
#include "src/project.h"

namespace llmcc {

struct ScoringMetadata {
  // Counts original source tokens, excluding a synthetic BOS token.
  std::size_t source_tokens = 0;
  std::uint32_t inference_context_tokens = 0;
  std::uint32_t window_stride_tokens = 0;
  std::size_t window_count = 0;
};

struct EntropyProviderResult {
  std::vector<EntropyRecord> records;
  ScoringMetadata metadata;
  bool entropy_cache_hit = false;
};

class EntropyProvider {
 public:
  virtual ~EntropyProvider() = default;
  virtual EntropyProviderResult Score(std::string_view source) = 0;
};

class ScorerInitializationError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// A shared entropy cache consulted before the local one, such as a
// comparison store. Entries are the native CBOR encoding under
// EntropyCacheKey; a failing Fetch or Store fails the file.
class EntropyTier {
 public:
  EntropyTier() = default;
  EntropyTier(const EntropyTier&) = delete;
  EntropyTier& operator=(const EntropyTier&) = delete;
  EntropyTier(EntropyTier&&) = delete;
  EntropyTier& operator=(EntropyTier&&) = delete;
  virtual ~EntropyTier() = default;
  virtual std::optional<std::string> Fetch(std::string_view key) = 0;
  virtual void Store(std::string_view key, std::string_view entry) = 0;
};

struct ProjectAnalysisOptions {
  ModelIdentity model;
  TauRule tau_rule;
  double alpha = 0.8;
  bool cache = true;
  std::size_t hotspots = 10;
  HierarchyMode hierarchy_mode = HierarchyMode::kStructural;
  std::uint32_t inference_context_tokens = 128U * 1024U;
  // Not owned; may be null.
  EntropyTier* tier = nullptr;
};

struct FunctionScore {
  std::string name;
  std::size_t start_line;
  std::size_t end_line;
  Metrics metrics;
};

struct Hotspot {
  std::size_t line;
  double max_entropy;
  double mean_entropy;
  std::uint64_t high_tokens;
};

struct FileAnalysisResult {
  Analysis analysis;
  bool entropy_cache_hit;
  ScoringMetadata scoring;
  std::vector<FunctionScore> functions;
  std::vector<Hotspot> hotspots;
};

class ProjectAnalyzer {
 public:
  using ProviderFactory = std::function<std::unique_ptr<EntropyProvider>()>;

  ProjectAnalyzer(ProjectAnalysisOptions options, ProviderFactory factory);
  FileAnalysisResult AnalyzeFile(const DiscoveredSource& source,
                                 std::string_view contents);
  [[nodiscard]] bool ScorerLoaded() const;

 private:
  EntropyProvider& Provider();
  EntropyProviderResult ReadRecords(std::string_view source);

  ProjectAnalysisOptions options_;
  ProviderFactory factory_;
  std::unique_ptr<EntropyProvider> provider_;
  std::optional<std::string> initialization_error_;
};

}  // namespace llmcc

#endif  // LLM_CC_ANALYZE_H_
