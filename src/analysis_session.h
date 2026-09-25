#ifndef LLM_CC_ANALYSIS_SESSION_H_
#define LLM_CC_ANALYSIS_SESSION_H_

#include <cstddef>
#include <memory>

#include "src/analyze.h"
#include "src/backend.h"
#include "src/model_identity.h"
#include "src/progress.h"
#include "src/scoring_settings.h"

namespace llmcc {

struct ScorerRequest {
  // Validated with ValidateScoringSettings.
  ScoringSettings settings;
  ModelRequest model;
  bool no_download = false;
  bool no_cache = false;
  bool backend_diagnostics = false;
  std::size_t hotspots = 10;
};

struct ScorerSession {
  BackendKind backend = BackendKind::kCpu;
  ModelIdentity identity;
  EffectiveTau tau;
  // Whether the shared entropy cache may be read and written.
  bool entropy_cache = false;
  // Loads the model on its first entropy-cache miss.
  std::unique_ptr<ProjectAnalyzer> analyzer;
};

// Selects the backend, resolves and inspects the model, and prepares an
// analyzer that loads the llama.cpp scorer lazily. Throws
// GpuRecoverableError when the requested GPU backend cannot be loaded.
ScorerSession OpenScorerSession(const ScorerRequest& request,
                                ProgressReporter& progress);

}  // namespace llmcc

#endif  // LLM_CC_ANALYSIS_SESSION_H_
