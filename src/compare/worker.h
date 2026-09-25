#ifndef LLM_CC_COMPARE_WORKER_H_
#define LLM_CC_COMPARE_WORKER_H_

#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "src/compare/execution_host.h"
#include "src/compare/store.h"
#include "src/progress.h"
#include "src/scorer_session.h"

namespace llmcc::compare {

inline constexpr std::chrono::seconds kDefaultWorkerDeadline{6600};

struct WorkerOptions {
  std::filesystem::path plan;
  int worker_id = 0;
  std::filesystem::path output;
  Store* store = nullptr;
  ScorerSessionFactory open_session;
  // The running build's inference ABI; a plan from another build is refused.
  std::string inference_abi;
  ModelRequest model;
  std::optional<std::filesystem::path> backend_directory;
  std::chrono::seconds deadline = kDefaultWorkerDeadline;
  // Bare-host execution: verify the GPU and hold its lock while scoring.
  std::optional<ExecutionHost> execution_host;
  HostPaths host_paths;
  ProgressReporter* progress = nullptr;
};

// Scores one worker's assignment with one model session, storing every
// result and its native entropy as soon as it is ready. worker-<id>.json is
// always written, with status "complete" only when every assigned file was
// scored. Returns the artifact.
nlohmann::json RunWorker(const WorkerOptions& options);

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_WORKER_H_
