#ifndef LLM_CC_ANALYZE_H_
#define LLM_CC_ANALYZE_H_

#include <functional>
#include <memory>
#include <stdexcept>
#include <string_view>

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
  double tau_percentile = 67.0;
  double alpha = 0.8;
  bool cache = true;
};

struct FileAnalysisResult {
  Analysis analysis;
  bool entropy_cache_hit;
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
};

}  // namespace llmcc

#endif  // LLM_CC_ANALYZE_H_
