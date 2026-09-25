#ifndef LLM_CC_ANALYSIS_SESSION_H_
#define LLM_CC_ANALYSIS_SESSION_H_

#include "src/progress.h"
#include "src/scorer_session.h"

namespace llmcc {

// Selects the backend, resolves and inspects the model, and prepares an
// analyzer that loads the llama.cpp scorer lazily. Throws
// GpuRecoverableError when the requested GPU backend cannot be loaded.
ScorerSession OpenScorerSession(const ScorerRequest& request,
                                ProgressReporter& progress);

}  // namespace llmcc

#endif  // LLM_CC_ANALYSIS_SESSION_H_
