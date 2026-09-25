// `llm-cc compare` end to end with the deterministic scorer.
#include "src/compare/cli.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "src/compare/fake_scorer.h"
#include "src/compare/json_util.h"
#include "src/compare/test_repository.h"
#include "src/test_util.h"

namespace {

namespace fs = std::filesystem;
using llmcc::compare::ReadJsonFile;
using llmcc::test::Expect;
using llmcc::test::ExpectEq;
using nlohmann::json;

int Compare(std::vector<std::string> arguments, std::string* output = nullptr) {
  arguments.insert(arguments.begin(), "compare");
  std::vector<char*> argv;
  argv.reserve(arguments.size());
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }
  std::ostringstream captured;
  std::streambuf* previous = std::cout.rdbuf(captured.rdbuf());
  const int status = llmcc::compare::RunCompareCommand(
      static_cast<int>(argv.size()), argv.data(),
      {.inference_abi = std::string(llmcc::compare::kFakeInferenceAbi),
       .open_session = llmcc::compare::DeterministicScorerFactory()});
  std::cout.rdbuf(previous);
  if (output != nullptr) {
    *output = captured.str();
  }
  return status;
}

// Report contents that do not depend on when or how a run was cached.
json Comparable(const fs::path& directory) {
  json report = ReadJsonFile(directory / "report.json");
  report.erase("cache_stats");
  report["identity"].erase("started_at");
  return report;
}

void SetEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
#endif
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  const char* temporary = std::getenv("TEST_TMPDIR");
  Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const fs::path root(temporary);
  // Model digest memos and the default store stay inside the test.
  SetEnvironment("LLM_CC_CACHE_DIR", (root / "cache/models").string());
  const auto repository =
      llmcc::compare::test::MakePipelineRepository(root / "repo");
  const std::string model = (root / "model.gguf").string();
  llmcc::compare::test::Write(model, "fake weights");
  const std::string store = (root / "store").string();
  const std::vector<std::string> revisions = {
      "--repo",   repository.root.string(), "--head", repository.head,
      "--target", repository.base};
  const auto run = [&](const std::string& output,
                       std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments = {"run"};
    arguments.insert(arguments.end(), revisions.begin(), revisions.end());
    for (const std::string& argument : std::vector<std::string>{
             "--output-dir", output, "--model", model, "--progress", "never"}) {
      arguments.push_back(argument);
    }
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return Compare(arguments);
  };

  // A local run resolves auto settings, scores every file and reports.
  const fs::path cold = root / "cold";
  Expect(run(cold.string(), {"--cache", store}) != 2, "compare run succeeds");
  const json plan = ReadJsonFile(cold / "plan.json");
  Expect(plan["scoring"]["backend"] == "cpu" &&
             plan["scoring"]["entropy_reduction"] == "host",
         "auto settings are resolved before planning");
  Expect(!plan["workers"].empty() &&
             ReadJsonFile(cold / "report.json")["status"] != "failed",
         "a cold run scores and reports");

  // A rerun is served from the cache with the same report.
  const fs::path warm = root / "warm";
  run(warm.string(), {"--cache", store});
  const json warm_plan = ReadJsonFile(warm / "plan.json");
  Expect(
      warm_plan["workers"].empty() && warm_plan["cache_stats"]["misses"] == 0,
      "a rerun misses nothing");
  ExpectEq(Comparable(warm), Comparable(cold), "a cached run reports the same");

  // Four workers produce the report one did.
  const fs::path four = root / "four";
  run(four.string(),
      {"--cache", (root / "four-store").string(), "--max-workers", "4"});
  Expect(ReadJsonFile(four / "plan.json")["workers"].size() > 1,
         "misses spread over several workers");
  ExpectEq(Comparable(four), Comparable(cold), "worker count is invisible");

  // Without --cache, results land beside the model cache.
  run((root / "default").string());
  Expect(fs::is_directory(root / "cache/compare/results/v2"),
         "the default store is compare/ beside the model cache");

  // The CI stages reproduce the local run from a pinned identity.
  const fs::path scoring = root / "scoring.args";
  llmcc::compare::test::Write(
      scoring,
      "# CPU scoring\n--backend\ncpu\n\n  --entropy-reduction  \nhost\n");
  std::string printed;
  ExpectEq(
      Compare({"identity", "--model", model, "@" + scoring.string()}, &printed),
      0, "identity prints the scorer identity");
  const json identity = json::parse(printed);
  ExpectEq(identity["fingerprint"], plan["fingerprint"],
           "explicit settings match the resolved local run");
  const fs::path staged = root / "staged";
  const fs::path identity_file = root / "identity.json";
  llmcc::compare::test::Write(identity_file,
                              R"({"repository": "o/r", "pipeline_id": "p"})");
  std::vector<std::string> prepare = {"prepare"};
  prepare.insert(prepare.end(), revisions.begin(), revisions.end());
  for (const std::string& argument : std::vector<std::string>{
           "--identity", identity_file.string(), "--output-dir",
           staged.string(), "--cache", (root / "staged-store").string(),
           "--max-workers", "2", "--model-sha256",
           identity["model"]["sha256"].get<std::string>(), "--model-bytes",
           identity["model"]["bytes"].dump(), "@" + scoring.string()}) {
    prepare.push_back(argument);
  }
  ExpectEq(Compare(prepare), 0, "prepare plans from a pinned model");
  const json staged_plan = ReadJsonFile(staged / "plan.json");
  std::vector<std::string> aggregate = {"aggregate", "--plan",
                                        (staged / "plan.json").string(),
                                        "--output-dir", staged.string()};
  for (const json& worker : staged_plan["workers"]) {
    const std::string id = worker["worker_id"].dump();
    ExpectEq(Compare({"worker", "--plan", (staged / "plan.json").string(),
                      "--worker-id", id, "--output-dir", staged.string(),
                      "--cache", (root / "staged-store").string(), "--model",
                      model, "--progress", "never"}),
             0, "each planned worker completes");
    aggregate.emplace_back("--worker");
    aggregate.push_back((staged / ("worker-" + id + ".json")).string());
  }
  Expect(Compare(aggregate) != 2, "aggregation succeeds");
  ExpectEq(ReadJsonFile(staged / "report.json")["results"],
           ReadJsonFile(cold / "report.json")["results"],
           "staged workers produce the local run's results");

  // A worker outside the plan fails but still reports.
  ExpectEq(
      Compare({"worker", "--plan", (staged / "plan.json").string(),
               "--worker-id", "3", "--output-dir", (root / "absent").string(),
               "--cache", store, "--model", model, "--progress", "never"}),
      1, "an unplanned worker exits 1");
  Expect(fs::exists(root / "absent/worker-3.json"),
         "an unplanned worker writes its artifact");

  // Usage errors exit 2 without planning.
  std::vector<std::string> automatic = prepare;
  automatic.pop_back();
  automatic.emplace_back("--output-dir");
  automatic.push_back((root / "automatic").string());
  ExpectEq(Compare(automatic), 2, "prepare refuses auto settings");
  Expect(!fs::exists(root / "automatic/plan.json"),
         "a refused prepare writes nothing");
  ExpectEq(Compare({"prepare", "--head", "HEAD"}), 2,
           "prepare requires its revisions and identity");
  ExpectEq(Compare({"run", "--target", "HEAD", "--bogus"}), 2,
           "unknown options are refused");
  ExpectEq(Compare({"identity", "--model", model, "--backend"}), 2,
           "a scoring option needs its value");
  const fs::path nested = root / "nested.args";
  llmcc::compare::test::Write(nested, "@" + scoring.string() + "\n");
  ExpectEq(Compare({"identity", "--model", model, "@" + nested.string()}), 2,
           "response files do not nest");
  ExpectEq(Compare({"identity", "--model", model, "--model-name", "x",
                    "@" + scoring.string()}),
           2, "one model source");
  ExpectEq(Compare({"frobnicate"}), 2, "unknown commands are refused");
  return 0;
}
