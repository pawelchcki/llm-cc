#include "src/compare/worker.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef __linux__
#include <unistd.h>
#endif

#include "src/analysis_totals.h"
#include "src/backend.h"
#include "src/compare/entropy_tier.h"
#include "src/compare/identity.h"
#include "src/compare/json_util.h"
#include "src/compare/plan.h"
#include "src/compare/result_cache.h"
#include "src/git.h"
#include "src/input_limits.h"
#include "src/lang.h"

namespace llmcc::compare {
namespace {

using nlohmann::json;
using Clock = std::chrono::steady_clock;

// Failures that end the whole worker rather than one file.
class WorkerAbort : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

std::string DescribeScorerMismatch(const json& planned, const json& running) {
  std::string fields;
  for (const auto& [name, value] : running.items()) {
    if (!planned.contains(name) || planned[name] != value) {
      fields += (fields.empty() ? "" : ", ") + name;
    }
  }
  return "the plan was prepared by a different llm-cc build (" +
         (fields.empty() ? std::string("fields differ") : fields) +
         "); coordinator and workers must run the same executable";
}

// Loader and GPU runtime settings change which libraries load or how kernels
// run, and neither the host contract nor the fingerprint records them, so a
// bare-host worker only accepts the visibility settings it applies itself.
void RefuseInheritedRuntimeSettings() {
#ifdef __linux__
  static constexpr std::array<std::string_view, 12> kPrefixes = {
      "LD_",  "HSA_",  "HIP_",    "ROCR_",    "ROCM_",      "AMD_",
      "GPU_", "CUDA_", "MIOPEN_", "ROCBLAS_", "HIPBLASLT_", "GGML_"};
  static constexpr std::array<std::string_view, 4> kNames = {
      "RUNFILES_DIR", "RUNFILES_MANIFEST_FILE", "LLM_CC_BACKEND_DIR",
      "LLM_CC_RUNTIME_DIR"};
  static constexpr std::array<std::string_view, 3> kApplied = {
      "ROCR_VISIBLE_DEVICES", "HIP_VISIBLE_DEVICES", "CUDA_VISIBLE_DEVICES"};
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view variable(*entry);
    const std::string_view name = variable.substr(0, variable.find('='));
    if (std::ranges::find(kApplied, name) != kApplied.end()) {
      continue;
    }
    if (std::ranges::find(kNames, name) != kNames.end() ||
        std::ranges::any_of(kPrefixes, [&](std::string_view prefix) {
          return name.starts_with(prefix);
        })) {
      throw WorkerAbort("bare-host workers run in a clean environment; unset " +
                        std::string(name));
    }
  }
#endif
}

void ApplyEnvironment(
    const std::vector<std::pair<std::string, std::string>>& environment) {
  for (const auto& [name, value] : environment) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
#endif
  }
}

std::string ReadVerifiedBlob(const std::filesystem::path& path,
                             const json& item) {
  const std::string blob_id = item["blob_id"];
  const auto size = item["size"].get<std::uint64_t>();
  std::string contents;
  try {
    contents = ReadBoundedFile(path, size);
  } catch (const std::exception&) {
    throw std::runtime_error("input blob " + blob_id +
                             " is missing or too large");
  }
  const git::ObjectFormat format = blob_id.size() == 64
                                       ? git::ObjectFormat::kSha256
                                       : git::ObjectFormat::kSha1;
  if (contents.size() != size || git::BlobId(contents, format) != blob_id) {
    throw std::runtime_error("input blob " + blob_id +
                             " does not match its object id");
  }
  return contents;
}

// Where each key appears, for messages that name a file.
std::map<std::string, std::string> PathsByKey(const json& plan) {
  std::map<std::string, std::string> paths;
  for (const char* side : {"base", "head"}) {
    for (const json& file : plan["inventories"][side]) {
      if (file["key"].is_string()) {
        paths.emplace(file["key"].get<std::string>(),
                      file["path"].get<std::string>());
      }
    }
  }
  return paths;
}

void Score(const WorkerOptions& options, const json& plan,
           const json& assignment, Clock::time_point deadline,
           ProgressReporter& progress, json& results,
           std::vector<std::string>& errors) {
  ScoringSettings settings = SettingsFromScoring(plan["scoring"]);
  settings.gpu_selection = options.gpu_selection;
  ValidateGpuSelection(settings.gpu_selection, settings.gpu_layers);
  RequireBuiltInBackends(settings);
  // Opening the scorer hashes the whole model, which cannot stop midway, so
  // an exhausted deadline is reported before starting it.
  const auto expired = [&] {
    if (Clock::now() <= deadline) {
      return false;
    }
    errors.push_back("worker deadline exceeded with " +
                     std::to_string(assignment["keys"].size()) +
                     " files unscored");
    return true;
  };
  if (expired()) {
    return;
  }
  std::unique_ptr<cache_io::FileLock> lease;
  if (options.execution_host.has_value()) {
    ApplyEnvironment(
        VerifyExecutionHost(*options.execution_host, options.host_paths));
    progress.Phase("waiting for the shared GPU");
    lease =
        AcquireGpuLease(*options.execution_host, options.host_paths, deadline);
    if (expired()) {
      return;
    }
  }
  StoreEntropyTier tier(*options.store, CanonicalDigest(plan["scorer"]));
  ScorerSession session =
      options.open_session(ScorerRequest{.settings = settings,
                                         .model = options.model,
                                         .no_download = true,
                                         .no_cache = true,
                                         .hotspots = 0,
                                         .tier = &tier,
                                         .require_model_digest = true},
                           progress);
  const ModelPin model = ModelFromJson(plan["model"]);
  if (session.identity.content_digest != model.sha256 ||
      session.identity.size != model.bytes) {
    throw WorkerAbort("the model's digest or size differs from the plan's " +
                      model.sha256);
  }
  if (plan["scoring"]["backend"] != session.identity.backend) {
    throw WorkerAbort("this worker would score with " +
                      session.identity.backend + ", not the planned " +
                      plan["scoring"]["backend"].get<std::string>());
  }
  const std::string fingerprint = plan["fingerprint"];
  ResultCache cache(*options.store);
  const std::map<std::string, std::string> paths = PathsByKey(plan);
  const std::filesystem::path blobs = options.plan.parent_path() / "blobs";
  const json& keys = assignment["keys"];
  std::size_t index = 0;
  for (const json& key_json : keys) {
    const std::string key = key_json;
    if (Clock::now() > deadline) {
      errors.push_back("worker deadline exceeded with " +
                       std::to_string(keys.size() - index) + " files unscored");
      break;
    }
    ++index;
    const json& item = plan["items"][key];
    const std::string& path = paths.at(key);
    progress.StartFile(index, keys.size(), path);
    try {
      const std::string contents =
          ReadVerifiedBlob(blobs / item["blob_id"].get<std::string>(), item);
      const DiscoveredSource source{
          .path = std::filesystem::u8path(path),
          .language = ParseLanguage(item["language"].get<std::string>()),
          .repository = std::nullopt,
          .relative_path = path};
      const FileAnalysisResult result =
          session.analyzer->AnalyzeFile(source, contents);
      json record =
          ResultRecord(item, fingerprint, FileTotalsOf(result.analysis));
      cache.Put(record);
      results[key] = std::move(record);
      progress.FinishFile(result.entropy_cache_hit);
    } catch (const ScorerInitializationError& error) {
      progress.FailFile();
      throw WorkerAbort(error.what());
    } catch (const GpuRecoverableError& error) {
      progress.FailFile();
      throw WorkerAbort(error.what());
    } catch (const StoreError& error) {
      progress.FailFile();
      throw WorkerAbort(error.what());
    } catch (const std::exception& error) {
      progress.FailFile();
      errors.push_back(path + ": " + error.what());
    }
  }
}

}  // namespace

json RunWorker(const WorkerOptions& options) {
  const Clock::time_point started = Clock::now();
  const Clock::time_point deadline = started + options.deadline;
  std::unique_ptr<ProgressReporter> silent;
  ProgressReporter* progress = options.progress;
  if (progress == nullptr) {
    silent = std::make_unique<ProgressReporter>("never");
    progress = silent.get();
  }
  json artifact = {{"schema_version", kSchemaVersion},
                   {"identity", nullptr},
                   {"fingerprint", nullptr},
                   {"worker_id", options.worker_id},
                   {"status", "failed"},
                   {"results", json::object()},
                   {"errors", json::array()},
                   {"elapsed_seconds", 0.0}};
  std::vector<std::string> errors;
  std::size_t assigned = 0;
  try {
    if (options.store == nullptr || !options.open_session) {
      throw std::invalid_argument("a worker needs a store and a scorer");
    }
    const json plan = ReadJsonFile(options.plan);
    ValidatePlan(plan);
    if (options.execution_host.has_value()) {
      RefuseInheritedRuntimeSettings();
    }
    artifact["identity"] = plan["identity"];
    artifact["fingerprint"] = plan["fingerprint"];
    const BackendKind backend = SettingsFromScoring(plan["scoring"]).backend;
    const json running = ScorerJson(
        options.inference_abi,
        {.execution_host =
             options.execution_host.has_value()
                 ? std::optional(ExecutionHostDigest(*options.execution_host))
                 : std::nullopt,
         .backend_artifact = options.backend_artifact
                                 ? options.backend_artifact(backend)
                                 : std::nullopt,
         .image = options.scorer_image});
    if (plan["scorer"] != running) {
      throw WorkerAbort(DescribeScorerMismatch(plan["scorer"], running));
    }
    const json* assignment = nullptr;
    for (const json& worker : plan["workers"]) {
      if (worker["worker_id"] == options.worker_id) {
        assignment = &worker;
      }
    }
    if (assignment == nullptr) {
      throw WorkerAbort("worker " + std::to_string(options.worker_id) +
                        " is absent from the plan");
    }
    assigned = (*assignment)["keys"].size();
    Score(options, plan, *assignment, deadline, *progress, artifact["results"],
          errors);
  } catch (const std::exception& error) {
    errors.emplace_back(error.what());
  }
  if (errors.empty() && assigned != 0 &&
      artifact["results"].size() == assigned) {
    artifact["status"] = "complete";
  }
  // Paths from arguments need not be UTF-8, and the artifact must encode.
  json escaped = json::array();
  for (const std::string& error : errors) {
    escaped.push_back(EscapeInvalidUtf8(error));
  }
  artifact["errors"] = std::move(escaped);
  artifact["elapsed_seconds"] =
      std::chrono::duration<double>(Clock::now() - started).count();
  WriteJsonFile(options.output /
                    ("worker-" + std::to_string(options.worker_id) + ".json"),
                artifact);
  return artifact;
}

}  // namespace llmcc::compare
