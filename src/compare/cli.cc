#include "src/compare/cli.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>

#include <cstdio>
#endif

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/cache.h"
#include "src/cache_io.h"
#include "src/compare/aggregate.h"
#include "src/compare/execution_host.h"
#include "src/compare/identity.h"
#include "src/compare/json_util.h"
#include "src/compare/prepare.h"
#include "src/compare/store.h"
#include "src/compare/worker.h"
#include "src/input_limits.h"
#include "src/progress.h"
#include "src/scoring_settings.h"

namespace llmcc::compare {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr std::string_view kUsage =
    "Usage:\n"
    "  llm-cc compare run --target REV --output-dir DIR [--head REV]\n"
    "      [--repo DIR] [--identity FILE] [--cache LOCATION]\n"
    "      [--store-options FILE] [--max-workers N] [--deadline-seconds N]\n"
    "      [--progress MODE] [PLANNING] [--model GGUF | --model-name NAME]\n"
    "      [SCORING]\n"
    "  llm-cc compare prepare --head REV --target REV --identity FILE\n"
    "      --output-dir DIR [--repo DIR] [--cache LOCATION]\n"
    "      [--store-options FILE] [--max-workers N] [PLANNING] MODEL SCORING\n"
    "  llm-cc compare worker --plan PLAN --worker-id N --output-dir DIR\n"
    "      --cache LOCATION [--store-options FILE]\n"
    "      [--model GGUF | --model-name NAME]\n"
    "      [--deadline-seconds N] [--execution-host FILE] [--progress MODE]\n"
    "  llm-cc compare aggregate --output-dir DIR [--plan PLAN] "
    "[--worker FILE]...\n"
    "      [--identity FILE] [--error MESSAGE]...\n"
    "  llm-cc compare identity MODEL SCORING\n"
    "  llm-cc compare store get KEY --cache LOCATION [--store-options FILE]\n"
    "      [--output FILE]\n"
    "  llm-cc compare store put KEY --cache LOCATION [--store-options FILE]\n"
    "      [--input FILE]\n\n"
    "run compares the merge base of --target and --head (default HEAD):\n"
    "it prepares a plan, scores every miss in this process and aggregates.\n"
    "Results are cached in LOCATION, a directory or s3://BUCKET/PREFIX\n"
    "(default: compare/ beside the model cache). Auto backend and entropy\n"
    "reduction are resolved on this machine.\n\n"
    "prepare, worker and aggregate are the same stages for CI, where a\n"
    "coordinator and GPU workers must run the same llm-cc build. prepare\n"
    "writes plan.json and the bytes of each miss to blobs/; worker N scores\n"
    "its assignment, stores each result as it lands and always writes\n"
    "worker-N.json; aggregate validates everything and writes report.json,\n"
    "report.md, comment.md, baseline.md, baseline.json and publication.json.\n"
    "Each --error makes the report a failure; without a plan, --identity\n"
    "names the pipeline it belongs to. identity prints the scorer, model,\n"
    "scoring and fingerprint a plan would use.\n\n"
    "MODEL is --model GGUF, --model-name NAME, or --model-sha256 HEX with\n"
    "--model-bytes N to pin weights this machine does not have. SCORING takes\n"
    "the analysis options (--backend, --gpu-layers, --context, --batch-size,\n"
    "--entropy-reduction, --flash-attn, --kv-cache-type, --kv-offload,\n"
    "--hierarchy, --tau, --tau-percentile, --alpha, --force-cpu); outside\n"
    "run, --backend, --entropy-reduction and --flash-attn must be explicit.\n"
    "PLANNING is --max-file-bytes N (default 65536), --default-rules FILE\n"
    "(rules when the target commit has none), --presentation FILE,\n"
    "--cache-concurrency N (default 8), --refresh-days N (default 20) and\n"
    "--expire-days N (default 30). --max-workers is at most 4 (default: 4\n"
    "for prepare, 1 for run).\n\n"
    "An argument @FILE in place of an option reads further arguments from\n"
    "FILE, one per line; blank lines and lines starting with # are ignored.\n\n"
    "worker --execution-host FILE verifies a bare-metal AMD GPU host and\n"
    "holds its lock while scoring. The deadline defaults to 6600 seconds.\n\n"
    "store reads or writes one object of a filesystem or s3://bucket/prefix\n"
    "store, using standard output and input by default. A missing object\n"
    "exits 1. S3 credentials come from AWS_ACCESS_KEY_ID,\n"
    "AWS_SECRET_ACCESS_KEY and AWS_SESSION_TOKEN.\n\n"
    "run, worker and aggregate exit 1 when the comparison failed.\n";

constexpr std::uintmax_t kMaxResponseFileBytes = std::uintmax_t{64} * 1024;

class CompareUsageError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

std::vector<std::string> ReadResponseFile(const fs::path& path) {
  std::string text;
  try {
    text = ReadBoundedFile(path, kMaxResponseFileBytes);
  } catch (const std::exception& error) {
    throw CompareUsageError("cannot read response file " +
                            cache_io::PathUtf8(path) + ": " + error.what());
  }
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      end = text.size();
    }
    std::string line = text.substr(start, end - start);
    start = end + 1;
    const std::size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos || line[first] == '#') {
      continue;
    }
    line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
    if (line.front() == '@') {
      throw CompareUsageError("response files cannot name another: " + line);
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

// Options and their values, with @FILE response files expanded wherever an
// option is expected.
class Arguments {
 public:
  explicit Arguments(const std::vector<std::string_view>& tokens)
      : tokens_(tokens.begin(), tokens.end()) {}

  bool More() {
    while (!tokens_.empty() && tokens_.front().size() > 1 &&
           tokens_.front().front() == '@') {
      const std::vector<std::string> lines =
          ReadResponseFile(fs::u8path(tokens_.front().substr(1)));
      tokens_.pop_front();
      tokens_.insert(tokens_.begin(), lines.begin(), lines.end());
    }
    return !tokens_.empty();
  }

  std::string Next() {
    std::string token = std::move(tokens_.front());
    tokens_.pop_front();
    return token;
  }

  std::string Value(std::string_view option) {
    if (tokens_.empty()) {
      throw CompareUsageError(std::string(option) + " requires a value");
    }
    return Next();
  }

  [[nodiscard]] const std::string* Peek() const {
    return tokens_.empty() ? nullptr : &tokens_.front();
  }

 private:
  std::deque<std::string> tokens_;
};

[[noreturn]] void UnknownOption(std::string_view command,
                                std::string_view option) {
  throw CompareUsageError("unknown " + std::string(command) +
                          " option: " + std::string(option));
}

// Applies one analysis scoring option, consuming its value.
bool ParseScoring(ScoringSettings& settings, std::string_view option,
                  Arguments& arguments) {
  if (ParseScoringFlag(settings, option)) {
    return true;
  }
  const std::string* value = arguments.Peek();
  if (value == nullptr) {
    ScoringSettings scratch;
    bool known = false;
    try {
      known = ParseScoringOption(scratch, option, "");
    } catch (const UsageError&) {
      known = true;
    }
    if (known) {
      throw CompareUsageError(std::string(option) + " requires a value");
    }
    return false;
  }
  if (!ParseScoringOption(settings, option, *value)) {
    return false;
  }
  arguments.Next();
  return true;
}

struct ModelArguments {
  ModelRequest request;
  std::optional<std::string> sha256;
  std::optional<std::uint64_t> bytes;
};

bool ParseModel(ModelArguments& model, std::string_view option,
                Arguments& arguments, bool allow_pin) {
  if (option == "--model") {
    model.request.model = fs::u8path(arguments.Value(option));
  } else if (option == "--model-name") {
    model.request.model_name = arguments.Value(option);
  } else if (allow_pin && option == "--model-sha256") {
    model.sha256 = arguments.Value(option);
  } else if (allow_pin && option == "--model-bytes") {
    model.bytes =
        ParseOptionNumber<std::uint64_t>(option, arguments.Value(option));
  } else {
    return false;
  }
  if (model.request.model.has_value() && model.request.model_name.has_value()) {
    throw CompareUsageError("--model and --model-name are mutually exclusive");
  }
  return true;
}

std::chrono::seconds ParseDeadline(std::string_view option,
                                   Arguments& arguments) {
  const auto seconds =
      ParseOptionNumber<std::int64_t>(option, arguments.Value(option));
  if (seconds < 0) {
    throw CompareUsageError(std::string(option) + " must not be negative");
  }
  return std::chrono::seconds(seconds);
}

std::string ParseProgress(std::string_view option, Arguments& arguments) {
  std::string mode = arguments.Value(option);
  if (mode != "auto" && mode != "always" && mode != "never") {
    throw CompareUsageError("--progress expects auto, always, or never");
  }
  return mode;
}

enum class Command : std::uint8_t { kIdentity, kPrepare, kRun };

std::string_view CommandName(Command command) {
  switch (command) {
    case Command::kIdentity:
      return "identity";
    case Command::kPrepare:
      return "prepare";
    case Command::kRun:
      return "run";
  }
  return "compare";
}

struct PlanArguments {
  PrepareOptions prepare;
  std::optional<fs::path> identity;
  std::string cache;
  std::optional<fs::path> store_options;
  std::optional<fs::path> presentation;
  ModelArguments model;
  ScoringSettings scoring;
  std::chrono::seconds deadline = kDefaultWorkerDeadline;
  std::string progress = "auto";
};

// Planning options shared by prepare and run; identity takes only the model
// and scoring.
bool ParsePlanning(PlanArguments& parsed, std::string_view option,
                   Arguments& arguments) {
  const auto integer = [&] {
    return ParseOptionNumber<int>(option, arguments.Value(option));
  };
  PrepareOptions& prepare = parsed.prepare;
  if (option == "--repo") {
    prepare.repository = fs::u8path(arguments.Value(option));
  } else if (option == "--head") {
    prepare.head = arguments.Value(option);
  } else if (option == "--target") {
    prepare.target = arguments.Value(option);
  } else if (option == "--identity") {
    parsed.identity = fs::u8path(arguments.Value(option));
  } else if (option == "--output-dir") {
    prepare.output = fs::u8path(arguments.Value(option));
  } else if (option == "--cache") {
    parsed.cache = arguments.Value(option);
  } else if (option == "--store-options") {
    parsed.store_options = fs::u8path(arguments.Value(option));
  } else if (option == "--cache-concurrency") {
    prepare.cache_concurrency = integer();
  } else if (option == "--refresh-days") {
    prepare.refresh_days = integer();
  } else if (option == "--expire-days") {
    prepare.expire_days = integer();
  } else if (option == "--max-workers") {
    prepare.max_workers = integer();
  } else if (option == "--max-file-bytes") {
    prepare.max_file_bytes =
        ParseOptionNumber<std::uint64_t>(option, arguments.Value(option));
  } else if (option == "--default-rules") {
    prepare.default_rules = fs::u8path(arguments.Value(option));
  } else if (option == "--presentation") {
    parsed.presentation = fs::u8path(arguments.Value(option));
  } else {
    return false;
  }
  return true;
}

PlanArguments ParsePlanArguments(const std::vector<std::string_view>& args,
                                 Command command) {
  PlanArguments parsed;
  if (command == Command::kRun) {
    parsed.prepare.head = "HEAD";
    parsed.prepare.max_workers = 1;
  }
  Arguments arguments(args);
  while (arguments.More()) {
    const std::string option = arguments.Next();
    if (ParseModel(parsed.model, option, arguments, command != Command::kRun) ||
        ParseScoring(parsed.scoring, option, arguments) ||
        (command != Command::kIdentity &&
         ParsePlanning(parsed, option, arguments))) {
      continue;
    }
    if (command == Command::kRun && option == "--deadline-seconds") {
      parsed.deadline = ParseDeadline(option, arguments);
    } else if (command == Command::kRun && option == "--progress") {
      parsed.progress = ParseProgress(option, arguments);
    } else {
      UnknownOption(CommandName(command), option);
    }
  }
  if (command == Command::kIdentity) {
    return parsed;
  }
  if (parsed.prepare.head.empty() || parsed.prepare.target.empty() ||
      parsed.prepare.output.empty()) {
    throw CompareUsageError(
        command == Command::kRun
            ? "run requires --target and --output-dir"
            : "prepare requires --head, --target, --identity and --output-dir");
  }
  return parsed;
}

struct ResolvedScoring {
  ScoringSettings settings;
  json scorer;
  json model;
  json scoring;
};

// The scorer, model and scoring a plan records. `resolve_auto` settles auto
// backend and entropy reduction with this machine's scorer, which run needs
// and the CI stages must not do: their coordinator is not the GPU host.
ResolvedScoring ResolveScoring(const CompareRuntime& runtime,
                               const PlanArguments& parsed, bool resolve_auto,
                               ProgressReporter& progress) {
  ScoringSettings settings = parsed.scoring;
  ValidateScoringSettings(settings);
  RequireBuiltInBackends(settings);
  const ModelArguments& model = parsed.model;
  if (model.sha256.has_value() != model.bytes.has_value()) {
    throw CompareUsageError("--model-sha256 and --model-bytes go together");
  }
  const bool pinned = model.sha256.has_value();
  if (pinned && (model.request.model.has_value() ||
                 model.request.model_name.has_value())) {
    throw CompareUsageError(
        "name the model once: --model, --model-name or --model-sha256");
  }
  if (!resolve_auto) {
    RequireExplicitScoring(settings);
  }
  ModelPin pin;
  if (pinned) {
    pin = ModelFromJson({{"sha256", *model.sha256}, {"bytes", *model.bytes}});
  } else {
    const ScorerSession session =
        runtime.open_session(ScorerRequest{.settings = settings,
                                           .model = model.request,
                                           .hotspots = 0,
                                           .require_model_digest = true},
                             progress);
    pin = {.sha256 = session.identity.content_digest,
           .bytes = session.identity.size};
    if (settings.backend == BackendKind::kAuto) {
      settings.backend = session.backend;
      ValidateScoringSettings(settings);
    }
    if (settings.entropy_reduction == EntropyReduction::kAuto) {
      settings.entropy_reduction =
          session.identity.effective_reducer == "device"
              ? EntropyReduction::kDevice
              : EntropyReduction::kHost;
    }
    RequireExplicitScoring(settings);
  }
  return {
      .settings = settings,
      .scorer = ScorerJson(runtime.inference_abi),
      .model = ModelJson(pin),
      .scoring = ScoringJson(settings, ComparisonTau(settings, pin.sha256))};
}

std::unique_ptr<Store> OpenCache(const std::string& location,
                                 const std::optional<fs::path>& options) {
  return OpenStore(location, ReadStoreOptions(options));
}

json ReadObject(const fs::path& path, std::string_view what) {
  json value = ReadJsonFile(path);
  if (!value.is_object()) {
    throw CompareUsageError(std::string(what) + " must be a JSON object");
  }
  return value;
}

PrepareOptions PlanOptions(const PlanArguments& parsed,
                           const ResolvedScoring& resolved, Store* cache,
                           json identity) {
  PrepareOptions options = parsed.prepare;
  options.identity = std::move(identity);
  options.cache = cache;
  options.presentation = parsed.presentation.has_value()
                             ? ReadObject(*parsed.presentation, "presentation")
                             : json::object();
  options.scorer = resolved.scorer;
  options.model = resolved.model;
  options.scoring = resolved.scoring;
  return options;
}

void PrintPlan(const json& plan, const fs::path& output) {
  const json& stats = plan["cache_stats"];
  std::cout << "planned " << stats["items"].get<std::uint64_t>()
            << " files: " << stats["hits"].get<std::uint64_t>() << " cached, "
            << stats["misses"].get<std::uint64_t>() << " to score on "
            << plan["workers"].size()
            << " workers: " << cache_io::PathUtf8(output / "plan.json") << '\n';
}

int RunIdentity(const std::vector<std::string_view>& args,
                const CompareRuntime& runtime) {
  const PlanArguments parsed = ParsePlanArguments(args, Command::kIdentity);
  ProgressReporter progress("auto");
  const ResolvedScoring resolved =
      ResolveScoring(runtime, parsed, false, progress);
  const json identity = {
      {"scorer", resolved.scorer},
      {"model", resolved.model},
      {"scoring", resolved.scoring},
      {"fingerprint",
       Fingerprint(resolved.scorer, resolved.model, resolved.scoring)}};
  std::cout << identity.dump(2) << '\n';
  return 0;
}

int RunPrepare(const std::vector<std::string_view>& args,
               const CompareRuntime& runtime) {
  const PlanArguments parsed = ParsePlanArguments(args, Command::kPrepare);
  const std::optional<fs::path>& identity = parsed.identity;
  if (!identity.has_value()) {
    throw CompareUsageError(
        "prepare requires --head, --target, --identity and --output-dir");
  }
  ProgressReporter progress("auto");
  const ResolvedScoring resolved =
      ResolveScoring(runtime, parsed, false, progress);
  const std::unique_ptr<Store> cache =
      parsed.cache.empty() ? nullptr
                           : OpenCache(parsed.cache, parsed.store_options);
  const PrepareOptions options = PlanOptions(parsed, resolved, cache.get(),
                                             ReadObject(*identity, "identity"));
  progress.Phase("preparing comparison plan");
  const json plan = Prepare(options);
  PrintPlan(plan, options.output);
  return 0;
}

std::string RepositoryName(const fs::path& repository) {
  const std::string name =
      cache_io::PathUtf8(fs::weakly_canonical(repository).filename());
  return name.empty() ? "repository" : name;
}

int RunLocal(const std::vector<std::string_view>& args,
             const CompareRuntime& runtime) {
  const PlanArguments parsed = ParsePlanArguments(args, Command::kRun);
  ProgressReporter progress(parsed.progress);
  const ResolvedScoring resolved =
      ResolveScoring(runtime, parsed, true, progress);
  const std::string location =
      parsed.cache.empty()
          ? cache_io::PathUtf8(CacheDir().parent_path() / "compare")
          : parsed.cache;
  const std::unique_ptr<Store> cache =
      OpenCache(location, parsed.store_options);
  const PrepareOptions options = PlanOptions(
      parsed, resolved, cache.get(),
      parsed.identity.has_value()
          ? ReadObject(*parsed.identity, "identity")
          : json{{"repository", RepositoryName(parsed.prepare.repository)},
                 {"pipeline_id", "local"}});
  progress.Phase("preparing comparison plan");
  const json plan = Prepare(options);
  PrintPlan(plan, options.output);
  const fs::path plan_path = options.output / "plan.json";
  std::vector<std::string> artifacts;
  for (const json& assignment : plan["workers"]) {
    const int worker_id = assignment["worker_id"];
    RunWorker({.plan = plan_path,
               .worker_id = worker_id,
               .output = options.output,
               .store = cache.get(),
               .open_session = runtime.open_session,
               .inference_abi = runtime.inference_abi,
               .model = parsed.model.request,
               .deadline = parsed.deadline,
               .progress = &progress});
    artifacts.push_back(cache_io::PathUtf8(
        options.output / ("worker-" + std::to_string(worker_id) + ".json")));
  }
  const json report = AggregatePlan(plan_path, artifacts, options.output);
  const std::string status = report["status"].get<std::string>();
  std::cout << "comparison " << status << ": "
            << cache_io::PathUtf8(options.output / "report.md") << '\n';
  return status == "failed" ? 1 : 0;
}

int RunWorkerCommand(const std::vector<std::string_view>& args,
                     const CompareRuntime& runtime) {
  WorkerOptions options{.open_session = runtime.open_session,
                        .inference_abi = runtime.inference_abi};
  std::optional<int> worker_id;
  std::string cache;
  std::optional<fs::path> store_options;
  std::optional<fs::path> execution_host;
  std::string progress_mode = "auto";
  ModelArguments model;
  Arguments arguments(args);
  while (arguments.More()) {
    const std::string option = arguments.Next();
    if (ParseModel(model, option, arguments, false)) {
      continue;
    }
    if (option == "--plan") {
      options.plan = fs::u8path(arguments.Value(option));
    } else if (option == "--worker-id") {
      worker_id = ParseOptionNumber<int>(option, arguments.Value(option));
    } else if (option == "--output-dir") {
      options.output = fs::u8path(arguments.Value(option));
    } else if (option == "--cache") {
      cache = arguments.Value(option);
    } else if (option == "--store-options") {
      store_options = fs::u8path(arguments.Value(option));
    } else if (option == "--deadline-seconds") {
      options.deadline = ParseDeadline(option, arguments);
    } else if (option == "--execution-host") {
      execution_host = fs::u8path(arguments.Value(option));
    } else if (option == "--progress") {
      progress_mode = ParseProgress(option, arguments);
    } else {
      UnknownOption("worker", option);
    }
  }
  if (options.plan.empty() || !worker_id.has_value() ||
      options.output.empty() || cache.empty()) {
    throw CompareUsageError(
        "worker requires --plan, --worker-id, --output-dir and --cache");
  }
  options.worker_id = *worker_id;
  options.model = model.request;
  if (execution_host.has_value()) {
    options.execution_host =
        ParseExecutionHost(ReadObject(*execution_host, "execution host"));
  }
  const std::unique_ptr<Store> store = OpenCache(cache, store_options);
  options.store = store.get();
  ProgressReporter progress(progress_mode);
  options.progress = &progress;
  const json artifact = RunWorker(options);
  const std::string status = artifact["status"].get<std::string>();
  std::cout << "worker " << *worker_id << ' ' << status << ": "
            << cache_io::PathUtf8(
                   options.output /
                   ("worker-" + std::to_string(*worker_id) + ".json"))
            << '\n';
  for (const json& error : artifact["errors"]) {
    std::cerr << "error: " << TerminalSafe(error.get<std::string>()) << '\n';
  }
  return status == "complete" ? 0 : 1;
}

struct AggregateArguments {
  std::optional<fs::path> plan;
  std::vector<std::string> workers;
  fs::path output;
  std::optional<fs::path> identity;
  std::vector<std::string> errors;
};

AggregateArguments ParseAggregate(const std::vector<std::string_view>& args) {
  AggregateArguments parsed;
  Arguments arguments(args);
  while (arguments.More()) {
    const std::string option = arguments.Next();
    const std::string value = arguments.Value(option);
    if (option == "--plan") {
      parsed.plan = fs::u8path(value);
    } else if (option == "--worker") {
      parsed.workers.push_back(value);
    } else if (option == "--output-dir") {
      parsed.output = fs::u8path(value);
    } else if (option == "--identity") {
      parsed.identity = fs::u8path(value);
    } else if (option == "--error") {
      parsed.errors.push_back(value);
    } else {
      UnknownOption("aggregate", option);
    }
  }
  if (parsed.output.empty()) {
    throw CompareUsageError("aggregate requires --output-dir");
  }
  if (!parsed.plan.has_value() && parsed.errors.empty()) {
    throw CompareUsageError("aggregate requires --plan or --error");
  }
  return parsed;
}

// A failure report still names its pipeline when the identity is readable.
json ReadIdentity(const std::optional<fs::path>& path) {
  if (!path.has_value()) {
    return nullptr;
  }
  try {
    return ReadJsonFile(*path);
  } catch (const std::exception& error) {
    std::cerr << "warning: " << TerminalSafe(error.what()) << '\n';
    return nullptr;
  }
}

int RunAggregate(const std::vector<std::string_view>& args) {
  const AggregateArguments arguments = ParseAggregate(args);
  json report;
  const std::optional<fs::path>& plan = arguments.plan;
  if (!plan.has_value()) {
    report =
        WriteFailureReport(arguments.output, ReadIdentity(arguments.identity),
                           nullptr, arguments.errors);
  } else if (arguments.errors.empty()) {
    report = AggregatePlan(*plan, arguments.workers, arguments.output);
  } else {
    // The run failed around a usable plan: keep the partial analysis next to
    // a failure report carrying both sets of errors.
    const json analysis =
        AggregatePlan(*plan, arguments.workers, arguments.output);
    WriteJsonFile(arguments.output / "analysis-report.json", analysis);
    std::vector<std::string> errors = arguments.errors;
    for (const json& error : analysis["errors"]) {
      errors.push_back(error.get<std::string>());
    }
    json identity = analysis["identity"];
    if (identity.is_null() ||
        (identity.is_object() &&
         identity.value("pipeline_id", json()).is_null())) {
      identity = ReadIdentity(arguments.identity);
    }
    report = WriteFailureReport(arguments.output, identity,
                                analysis["fingerprint"], errors);
  }
  const std::string status = report["status"].get<std::string>();
  std::cout << "comparison " << status << ": "
            << cache_io::PathUtf8(arguments.output) << '\n';
  return status == "failed" ? 1 : 0;
}

struct StoreArguments {
  std::string action;
  std::string key;
  std::string cache;
  std::optional<fs::path> store_options;
  std::optional<fs::path> file;
};

StoreArguments ParseStore(const std::vector<std::string_view>& args) {
  if (args.size() < 2 || (args[0] != "get" && args[0] != "put")) {
    throw CompareUsageError("store requires get or put and a KEY");
  }
  StoreArguments parsed{.action = std::string(args[0]),
                        .key = std::string(args[1])};
  Arguments arguments({args.begin() + 2, args.end()});
  while (arguments.More()) {
    const std::string option = arguments.Next();
    const std::string value = arguments.Value(option);
    if (option == "--cache") {
      parsed.cache = value;
    } else if (option == "--store-options") {
      parsed.store_options = fs::u8path(value);
    } else if ((option == "--output" && parsed.action == "get") ||
               (option == "--input" && parsed.action == "put")) {
      parsed.file = fs::u8path(value);
    } else {
      UnknownOption("store", option);
    }
  }
  if (parsed.cache.empty()) {
    throw CompareUsageError("store requires --cache");
  }
  return parsed;
}

int RunStore(const std::vector<std::string_view>& args) {
  const StoreArguments arguments = ParseStore(args);
#ifdef _WIN32
  // Objects are bytes; the console must not translate line endings.
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  const auto store = OpenCache(arguments.cache, arguments.store_options);
  if (arguments.action == "put") {
    const std::string value = arguments.file.has_value()
                                  ? ReadBoundedFile(*arguments.file)
                                  : ReadBoundedStream(std::cin);
    store->Put(arguments.key, value);
    return 0;
  }
  const std::optional<std::string> value = store->Get(arguments.key);
  if (!value.has_value()) {
    std::cerr << "not found: " << TerminalSafe(arguments.key) << '\n';
    return 1;
  }
  if (arguments.file.has_value()) {
    WriteFileAtomic(*arguments.file, *value);
  } else {
    std::cout.write(value->data(), static_cast<std::streamsize>(value->size()));
  }
  return 0;
}

}  // namespace

int RunCompareCommand(int argc, char** argv, const CompareRuntime& runtime) {
  std::vector<std::string_view> arguments(argv + 1, argv + argc);
  if (arguments.empty() || arguments[0] == "-h" || arguments[0] == "--help") {
    (arguments.empty() ? std::cerr : std::cout) << kUsage;
    return arguments.empty() ? 2 : 0;
  }
  const std::string_view action = arguments.front();
  arguments.erase(arguments.begin());
  try {
    if (!arguments.empty() &&
        (arguments.front() == "-h" || arguments.front() == "--help")) {
      std::cout << kUsage;
      return 0;
    }
    if (action == "run") {
      return RunLocal(arguments, runtime);
    }
    if (action == "prepare") {
      return RunPrepare(arguments, runtime);
    }
    if (action == "worker") {
      return RunWorkerCommand(arguments, runtime);
    }
    if (action == "aggregate") {
      return RunAggregate(arguments);
    }
    if (action == "identity") {
      return RunIdentity(arguments, runtime);
    }
    if (action == "store") {
      return RunStore(arguments);
    }
    throw CompareUsageError("unknown compare command: " + std::string(action));
  } catch (const CompareUsageError& error) {
    std::cerr << "error: " << error.what() << "\n\n" << kUsage;
    return 2;
  } catch (const UsageError& error) {
    std::cerr << "error: " << error.what() << "\n\n" << kUsage;
    return 2;
  } catch (const StoreError& error) {
    std::cerr << "error: " << TerminalSafe(error.what()) << '\n';
    return 2;
  }
}

}  // namespace llmcc::compare
