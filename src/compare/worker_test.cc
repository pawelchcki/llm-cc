// Worker behavior ported from the Python comparison tests.
#include "src/compare/worker.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "src/compare/entropy_tier.h"
#include "src/compare/fake_scorer.h"
#include "src/compare/identity.h"
#include "src/compare/json_util.h"
#include "src/compare/plan.h"
#include "src/compare/prepare.h"
#include "src/compare/result_cache.h"
#include "src/compare/store.h"
#include "src/compare/test_repository.h"
#include "src/model_identity.h"
#include "src/scoring_settings.h"
#include "src/test_util.h"

namespace {

namespace fs = std::filesystem;
using llmcc::test::Expect;
using llmcc::test::ExpectEq;
using nlohmann::json;

struct Harness {
  fs::path root;
  fs::path model;
  llmcc::compare::FilesystemStore store;
  std::shared_ptr<std::atomic<int>> scored =
      std::make_shared<std::atomic<int>>(0);

  explicit Harness(const fs::path& directory)
      : root(directory),
        model(directory / "model.gguf"),
        store(directory / "store") {
    llmcc::compare::test::Write(model, "fake weights");
  }

  [[nodiscard]] llmcc::compare::ModelPin Pin() const {
    const auto identity =
        llmcc::InspectModel(model, llmcc::compare::kFakeInferenceAbi, "cpu", 8,
                            8, "host", "host", false, "on", "q8_0", true, true);
    return {.sha256 = identity.content_digest, .bytes = identity.size};
  }

  json Prepare(const llmcc::compare::test::PipelineRepository& repository,
               const fs::path& output, int workers, double tau = 0.67,
               const std::string& head = {}) {
    llmcc::ScoringSettings settings;
    for (const auto& [option, value] :
         std::vector<std::pair<const char*, const char*>>{
             {"--backend", "cpu"}, {"--entropy-reduction", "host"}}) {
      llmcc::ParseScoringOption(settings, option, value);
    }
    llmcc::ValidateScoringSettings(settings);
    return llmcc::compare::Prepare(
        {.repository = repository.root,
         .head = head.empty() ? repository.head : head,
         .target = repository.base,
         .identity = {{"repository", "o/r"}, {"pipeline_id", "p"}},
         .output = output,
         .cache = &store,
         .max_workers = workers,
         .max_file_bytes = 1024,
         .scorer =
             llmcc::compare::ScorerJson(llmcc::compare::kFakeInferenceAbi),
         .model = llmcc::compare::ModelJson(Pin()),
         .scoring = llmcc::compare::ScoringJson(settings, tau)});
  }

  llmcc::compare::WorkerOptions Worker(const fs::path& plan, int worker_id,
                                       const fs::path& output) {
    return {.plan = plan,
            .worker_id = worker_id,
            .output = output,
            .store = &store,
            .open_session = llmcc::compare::DeterministicScorerFactory(scored),
            .inference_abi = std::string(llmcc::compare::kFakeInferenceAbi),
            .model = {.model = model}};
  }

  // Runs every planned worker and returns the union of their results.
  json RunAll(const json& plan, const fs::path& directory) {
    json results = json::object();
    for (const json& worker : plan["workers"]) {
      const json artifact = llmcc::compare::RunWorker(
          Worker(directory / "plan.json", worker["worker_id"], directory));
      Expect(artifact["status"] == "complete",
             "worker completes: " + artifact["errors"].dump());
      results.update(artifact["results"]);
    }
    return results;
  }
};

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  const char* temporary = std::getenv("TEST_TMPDIR");
  Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const fs::path root(temporary);
  const auto repository =
      llmcc::compare::test::MakePipelineRepository(root / "repo");
  Harness harness(root);

  // Cold: every miss is scored once, cached per file, with its entropy.
  const json cold = harness.Prepare(repository, root / "cold", 1);
  Expect(!cold["workers"].empty(), "a cold cache plans workers");
  const json cold_results = harness.RunAll(cold, root / "cold");
  ExpectEq(cold_results.size(), cold["items"].size(), "every item is measured");
  const int cold_scored = harness.scored->load();
  Expect(
      cold_scored > 0 && std::cmp_less_equal(cold_scored, cold["items"].size()),
      "each file reaches the scorer at most once");
  llmcc::compare::ResultCache cache(harness.store);
  for (const auto& [key, result] : cold_results.items()) {
    Expect(llmcc::compare::ValidResult(result, cold["items"][key],
                                       cold["fingerprint"]),
           "worker results are valid records");
    ExpectEq(cache.Get(cold["items"][key], cold["fingerprint"]),
             std::optional<json>(result), "each result is stored");
  }
  Expect(fs::exists(root / "store/entropy/v2") &&
             !fs::is_empty(root / "store/entropy/v2"),
         "native entropy entries are shared");

  // Warm: nothing left to score.
  const json warm = harness.Prepare(repository, root / "warm", 4);
  Expect(warm["workers"].empty() && warm["hits"] == cold_results,
         "a warm cache reuses every result");

  // A new threshold changes every result key, but entropy is reused.
  harness.scored->store(0);
  const json retuned = harness.Prepare(repository, root / "retuned", 4, 0.5);
  Expect(retuned["cache_stats"]["hits"] == 0 && !retuned["workers"].empty(),
         "a new fingerprint misses every result");
  const json retuned_results = harness.RunAll(retuned, root / "retuned");
  ExpectEq(harness.scored->load(), 0,
           "shared entropy entries spare inference entirely");
  ExpectEq(retuned_results.size(), retuned["items"].size(),
           "results are rebuilt from shared entropy");

  // One or four workers produce identical results.
  llmcc::compare::FilesystemStore fresh(root / "fresh-store");
  Harness four_workers(root / "four");
  const json four = four_workers.Prepare(repository, root / "four/plan", 4);
  Expect(four["workers"].size() > 1, "four workers split the misses");
  ExpectEq(four_workers.RunAll(four, root / "four/plan"), cold_results,
           "worker count never changes results");

  // Incremental: only the new file is scored.
  llmcc::compare::test::Write(repository.root / "incremental.go",
                              "package p\n\nfunc F() int { return 1 }\n");
  const std::string newer =
      llmcc::compare::test::Commit(repository.root, "incremental");
  const json incremental =
      harness.Prepare(repository, root / "incremental", 4, 0.67, newer);
  ExpectEq(incremental["cache_stats"]["misses"], json(1),
           "only the new file misses");

  // A zero deadline still writes a failed artifact.
  auto expired = harness.Worker(root / "cold/plan.json", 0, root / "expired");
  expired.deadline = std::chrono::seconds(0);
  const json late = llmcc::compare::RunWorker(expired);
  Expect(late["status"] == "failed" &&
             late["errors"][0].get<std::string>().find("deadline") !=
                 std::string::npos &&
             fs::exists(root / "expired/worker-0.json"),
         "an expired worker still reports");

  // Wrong weights, builds and assignments are refused.
  llmcc::compare::test::Write(root / "other.gguf", "other weights");
  auto wrong_model = harness.Worker(root / "cold/plan.json", 0, root / "wrong");
  wrong_model.model = {.model = root / "other.gguf"};
  const json mismatched = llmcc::compare::RunWorker(wrong_model);
  Expect(mismatched["status"] == "failed" &&
             mismatched["errors"][0].get<std::string>().find("digest") !=
                 std::string::npos &&
             mismatched["results"].empty(),
         "a model with other weights is refused");
  auto other_build = harness.Worker(root / "cold/plan.json", 0, root / "build");
  other_build.inference_abi = "llama.cpp-other/entropy-v9";
  const json foreign = llmcc::compare::RunWorker(other_build);
  Expect(foreign["status"] == "failed" &&
             foreign["errors"][0].get<std::string>().find(
                 "different llm-cc build") != std::string::npos,
         "a plan from another build is refused");
  const json absent = llmcc::compare::RunWorker(
      harness.Worker(root / "cold/plan.json", 3, root / "absent"));
  Expect(
      absent["status"] == "failed" && fs::exists(root / "absent/worker-3.json"),
      "an unplanned worker id fails and reports");

  // A tampered blob fails its file, not the whole worker.
  const json cold_again =
      harness.Prepare(repository, root / "tampered", 1, 0.3);
  const std::string victim = cold_again["workers"][0]["keys"][0];
  const std::string victim_blob = cold_again["items"][victim]["blob_id"];
  llmcc::compare::test::Write(root / "tampered/blobs" / victim_blob, "x");
  const json partial = llmcc::compare::RunWorker(
      harness.Worker(root / "tampered/plan.json", 0, root / "tampered"));
  Expect(partial["status"] == "failed" &&
             !partial["results"].contains(victim) &&
             partial["results"].size() + 1 ==
                 cold_again["workers"][0]["keys"].size(),
         "a blob that does not match its id is not scored");

  // A corrupt shared entropy entry is rescored rather than trusted.
  for (const auto& entry : fs::directory_iterator(root / "store/entropy/v2")) {
    llmcc::compare::test::Write(entry.path(), "corrupt");
  }
  harness.scored->store(0);
  const json rescored = harness.Prepare(repository, root / "rescored", 1, 0.9);
  harness.RunAll(rescored, root / "rescored");
  Expect(harness.scored->load() > 0, "corrupt entropy entries are rescored");
  return 0;
}
