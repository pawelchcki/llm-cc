#ifndef LLM_CC_SCORER_SESSION_H_
#define LLM_CC_SCORER_SESSION_H_

#include <cstddef>
#include <functional>
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
  // A shared entropy cache for the analyzer; not owned, may be null.
  EntropyTier* tier = nullptr;
  // Hashes the model even without a cache or calibration that needs it.
  bool require_model_digest = false;
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

// Opens a session; the llama.cpp implementation is OpenScorerSession.
using ScorerSessionFactory =
    std::function<ScorerSession(const ScorerRequest&, ProgressReporter&)>;

}  // namespace llmcc

#endif  // LLM_CC_SCORER_SESSION_H_
