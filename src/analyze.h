#ifndef LLM_CC_ANALYZE_H_
#define LLM_CC_ANALYZE_H_

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

class EntropyProvider {
 public:
  virtual ~EntropyProvider() = default;
  virtual std::vector<EntropyRecord> Score(std::string_view source) = 0;
};

class ScorerInitializationError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct ProjectAnalysisOptions {
  ModelIdentity model;
  TauRule tau_rule;
  double alpha = 0.8;
  bool cache = true;
  std::size_t hotspots = 10;
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
  ProjectAnalysisOptions options_;
  ProviderFactory factory_;
  std::unique_ptr<EntropyProvider> provider_;
  std::optional<std::string> initialization_error_;
};

}  // namespace llmcc

#endif  // LLM_CC_ANALYZE_H_
