// Preparation behavior ported from the Python comparison tests.
#include "src/compare/prepare.h"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/compare/identity.h"
#include "src/compare/json_util.h"
#include "src/compare/plan.h"
#include "src/compare/result_cache.h"
#include "src/compare/store.h"
#include "src/compare/test_repository.h"
#include "src/git.h"
#include "src/input_limits.h"
#include "src/rules.h"
#include "src/scoring_settings.h"
#include "src/test_util.h"

namespace {

namespace fs = std::filesystem;
using llmcc::compare::PrepareOptions;
using llmcc::compare::test::Commit;
using llmcc::compare::test::Git;
using llmcc::compare::test::Write;
using llmcc::test::Expect;
using llmcc::test::ExpectEq;
using nlohmann::json;

json Scoring() {
  llmcc::ScoringSettings settings;
  llmcc::ParseScoringOption(settings, "--backend", "cpu");
  llmcc::ParseScoringOption(settings, "--entropy-reduction", "host");
  llmcc::ValidateScoringSettings(settings);
  return llmcc::compare::ScoringJson(settings, 0.67);
}

PrepareOptions Options(const fs::path& repository, const std::string& head,
                       const std::string& target, const fs::path& output,
                       llmcc::compare::Store* cache = nullptr) {
  return {.repository = repository,
          .head = head,
          .target = target,
          .identity = {{"repository", "o/r"},
                       {"pipeline_id", "p"},
                       {"target_branch", "main"},
                       {"pr_number", 1},
                       {"started_at", "2026-01-01T00:00:00Z"}},
          .output = output,
          .cache = cache,
          .max_workers = 1,
          .max_file_bytes = 10,
          .scorer = llmcc::compare::ScorerJson("test-abi"),
          .model = llmcc::compare::ModelJson(
              {.sha256 = std::string(64, 'a'), .bytes = 1}),
          .scoring = Scoring()};
}

std::map<std::string, json> ByPath(const json& inventory) {
  std::map<std::string, json> paths;
  for (const json& file : inventory) {
    paths[file["path"].get<std::string>()] = file;
  }
  return paths;
}

std::set<std::string> AssignedKeys(const json& plan) {
  std::set<std::string> keys;
  for (const json& worker : plan["workers"]) {
    for (const json& key : worker["keys"]) {
      keys.insert(key.get<std::string>());
    }
  }
  return keys;
}

// Fills the cache with a result for every item of `plan`.
void CacheEverything(const json& plan, llmcc::compare::Store& store) {
  llmcc::compare::ResultCache cache(store);
  for (const auto& [key, item] : plan["items"].items()) {
    static_cast<void>(key);
    cache.Put(llmcc::compare::ResultRecord(item, plan["fingerprint"],
                                           {.llm_cc = 1.0, .token_count = 1}));
  }
}

template <typename Exception>
std::string Failure(const std::function<void()>& function) {
  try {
    function();
  } catch (const Exception& error) {
    return error.what();
  }
  return {};
}

// A store whose reads always fail.
class BrokenStore : public llmcc::compare::Store {
 public:
  std::optional<std::string> Get(std::string_view /*key*/) override {
    throw llmcc::compare::StoreError("access denied");
  }
  void Put(std::string_view /*key*/, std::string_view /*contents*/) override {}
  [[nodiscard]] std::string Describe() const override { return "broken"; }
};

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  const char* temporary = std::getenv("TEST_TMPDIR");
  Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const fs::path root(temporary);
  const auto repository =
      llmcc::compare::test::MakePipelineRepository(root / "repo");
  const auto& repo = repository.root;

  // Inventory: categories, exclusions, oversize, symlinks, deduplication.
  const json plan = llmcc::compare::Prepare(
      Options(repo, repository.head, repository.base, root / "prep"));
  const auto head = ByPath(plan["inventories"]["head"]);
  ExpectEq(head.at("src_test.cc")["category"], json("tests"),
           "test files are classified as tests");
  ExpectEq(head.at("tests/move.cc")["category"], json("tests"),
           "test directories are classified as tests");
  ExpectEq(head.at("large.py")["reason"], json("oversized"),
           "files above max_file_bytes are oversized");
  ExpectEq(head.at("README.md")["reason"], json("unsupported"),
           "unsupported languages are unscorable");
  ExpectEq(head.at("link.cc")["reason"], json("symlink"),
           "symbolic links are never followed");
  ExpectEq(head.at("api.hpp")["language"], json("cpp"),
           "headers resolve their language");
#ifndef _WIN32
  Expect(head.at("odd\nname.py")["scorable"] == true,
         "hostile file names are scored");
#endif
  std::size_t scorable = 0;
  for (const char* side : {"base", "head"}) {
    for (const json& file : plan["inventories"][side]) {
      scorable += file["scorable"].get<bool>() ? 1 : 0;
    }
  }
  Expect(plan["items"].size() < scorable,
         "identical contents share one item across paths and sides");
  ExpectEq(ByPath(plan["inventories"]["base"]).at("same.cc")["key"],
           ByPath(plan["inventories"]["base"]).at("copy.cc")["key"],
           "equal bytes in equal languages share a key");
  Expect(plan["identity"]["base_sha"] == repository.base &&
             plan["identity"]["head_sha"] == repository.head,
         "the identity records the commits compared");
  ExpectEq(plan["rules_source"], json({{"source", "builtin"}}),
           "without repository or host rules the built-ins apply");
  llmcc::compare::ValidatePlan(plan);
  for (const auto& [key, item] : plan["items"].items()) {
    static_cast<void>(key);
    const fs::path blob =
        root / "prep/blobs" / item["blob_id"].get<std::string>();
    Expect(fs::exists(blob) &&
               item["blob_id"] ==
                   llmcc::git::BlobId(llmcc::compare::ReadFileBytes(blob),
                                      llmcc::git::ObjectFormat::kSha1),
           "every miss has its verified blob");
  }

  // A warm cache plans no workers at all.
  llmcc::compare::FilesystemStore store(root / "store");
  CacheEverything(plan, store);
  const json warm = llmcc::compare::Prepare(
      Options(repo, repository.head, repository.base, root / "warm", &store));
  Expect(warm["workers"].empty() && warm["cache_stats"]["misses"] == 0 &&
             warm["cache_stats"]["hits"] == plan["items"].size(),
         "a fully cached plan needs no workers");

  // Another scorer cannot reuse those results.
  PrepareOptions other =
      Options(repo, repository.head, repository.base, root / "other", &store);
  other.scorer["version"] = "another build";
  const json foreign = llmcc::compare::Prepare(other);
  Expect(foreign["cache_stats"]["hits"] == 0 &&
             foreign["fingerprint"] != plan["fingerprint"],
         "a different scorer misses every cached result");

  // A later preparation repairs a truncated blob.
  const std::string first_blob =
      plan["items"].begin().value()["blob_id"].get<std::string>();
  const fs::path truncated = root / "prep/blobs" / first_blob;
  const std::string original = llmcc::compare::ReadFileBytes(truncated);
  Write(truncated, original.substr(0, original.size() / 2));
  static_cast<void>(llmcc::compare::Prepare(
      Options(repo, repository.head, repository.base, root / "prep")));
  ExpectEq(llmcc::compare::ReadFileBytes(truncated), original,
           "preparation rewrites the blobs it plans");

  // Partitions are stable across worker counts, orders and repetitions.
  PrepareOptions four =
      Options(repo, repository.head, repository.base, root / "four");
  four.max_workers = 4;
  const json four_plan = llmcc::compare::Prepare(four);
  PrepareOptions reversed =
      Options(repo, repository.base, repository.head, root / "reversed");
  reversed.max_workers = 4;
  const json reversed_plan = llmcc::compare::Prepare(reversed);
  ExpectEq(AssignedKeys(plan), AssignedKeys(four_plan),
           "one and four workers cover the same files");
  ExpectEq(AssignedKeys(four_plan), AssignedKeys(reversed_plan),
           "swapping sides covers the same files");
  ExpectEq(llmcc::compare::Prepare(four)["workers"], four_plan["workers"],
           "partitions are deterministic");
  std::uint64_t largest = 0;
  for (const json& worker : four_plan["workers"]) {
    largest = std::max(largest, worker["bytes"].get<std::uint64_t>());
  }
  Expect(four_plan["workers"].size() > 1 && largest < 30,
         "misses spread over workers by size");

  // Cache lookups are concurrent but the plan is not affected by it.
  PrepareOptions sequential = Options(repo, repository.head, repository.base,
                                      root / "sequential", &store);
  sequential.cache_concurrency = 1;
  PrepareOptions concurrent = sequential;
  concurrent.output = root / "concurrent";
  concurrent.cache_concurrency = 16;
  ExpectEq(llmcc::compare::Prepare(sequential),
           llmcc::compare::Prepare(concurrent),
           "concurrency never changes the plan");
  BrokenStore broken;
  Expect(
      Failure<llmcc::compare::StoreError>([&] {
        static_cast<void>(llmcc::compare::Prepare(Options(
            repo, repository.head, repository.base, root / "broken", &broken)));
      }).find("access denied") != std::string::npos,
      "a failing store fails preparation");

  // Rules come from the target commit, never from the head.
  Write(repo / ".llm-cc/comparison-rules.json", R"({"tooling": ["copy.cc"]})");
  const std::string ruled_target = Commit(repo, "target rules");
  Write(repo / ".llm-cc/comparison-rules.json", R"({"exclude": ["**"]})");
  const std::string ruled_head = Commit(repo, "head rules");
  const json ruled = llmcc::compare::Prepare(
      Options(repo, ruled_head, ruled_target, root / "ruled"));
  ExpectEq(ruled["rules_source"],
           json({{"source", "repository"},
                 {"path", ".llm-cc/comparison-rules.json"},
                 {"commit", ruled_target}}),
           "the target commit's legacy rules file applies");
  ExpectEq(ByPath(ruled["inventories"]["head"]).at("copy.cc")["category"],
           json("tooling"), "target rules classify the head");
  Write(repo / ".llm-cc/rules.json", R"({"tests": ["copy.cc"]})");
  const std::string preferred = Commit(repo, "shared rules name");
  const json shared = llmcc::compare::Prepare(
      Options(repo, preferred, preferred, root / "shared"));
  ExpectEq(ByPath(shared["inventories"]["head"]).at("copy.cc")["category"],
           json("tests"), ".llm-cc/rules.json wins over the legacy name");
  Write(repo / ".llm-cc/rules.json",
        R"({"tests": ["ok"], "unexpected": true})");
  const std::string invalid = Commit(repo, "invalid rules");
  Expect(Failure<llmcc::RulesError>([&] {
           static_cast<void>(llmcc::compare::Prepare(
               Options(repo, invalid, invalid, root / "invalid")));
         }).find("unsupported classification rule keys") != std::string::npos,
         "invalid repository rules fail the run");
  Write(repo / ".llm-cc/rules.json", "{not json");
  const std::string unparsable = Commit(repo, "unparsable rules");
  Expect(Failure<llmcc::RulesError>([&] {
           static_cast<void>(llmcc::compare::Prepare(
               Options(repo, unparsable, unparsable, root / "unparsable")));
         }).find("not valid JSON") != std::string::npos,
         "unparsable repository rules fail the run");
  fs::remove_all(repo / ".llm-cc");
  const std::string plain = Commit(repo, "no rules");
  Write(root / "host-rules.json", R"({"tooling": ["same.cc"]})");
  PrepareOptions hosted = Options(repo, plain, plain, root / "hosted");
  hosted.default_rules = root / "host-rules.json";
  const json host_plan = llmcc::compare::Prepare(hosted);
  Expect(
      host_plan["rules_source"] == json({{"source", "host"}}) &&
          ByPath(host_plan["inventories"]["head"]).at("same.cc")["category"] ==
              "tooling",
      "host rules apply when the target has none");

  // Path overrides select languages before keys, per side.
  const fs::path overrides = root / "overrides";
  fs::create_directories(overrides);
  Git(overrides, {"init", "-q"});
  Write(overrides / "c/x.h", "int shared(void);\n");
  Write(overrides / "cpp/x.h", "int shared(void);\n");
  Write(overrides / "c/moved.h", "int moved(void);\n");
  Write(overrides / ".llm-cc/rules.json",
        R"({"paths": [{"pattern": "cpp/**", "language": "cpp"},
                      {"pattern": "c/**", "language": "c"}]})");
  const std::string override_base = Commit(overrides, "base");
  Git(overrides, {"mv", "c/moved.h", "cpp/moved.h"});
  Write(overrides / ".llm-cc/rules.json",
        R"({"paths": [{"pattern": "**", "language": "rust"}]})");
  const std::string override_head = Commit(overrides, "move header");
  PrepareOptions override_options =
      Options(overrides, override_head, override_base, root / "override-plan");
  override_options.max_file_bytes = 1024;
  const json moved = llmcc::compare::Prepare(override_options);
  const auto base_files = ByPath(moved["inventories"]["base"]);
  const auto head_files = ByPath(moved["inventories"]["head"]);
  Expect(base_files.at("c/x.h")["language"] == "c" &&
             head_files.at("cpp/x.h")["language"] == "cpp",
         "path overrides choose the language, from the target's rules");
  Expect(base_files.at("c/x.h")["blob_id"] ==
                 base_files.at("cpp/x.h")["blob_id"] &&
             base_files.at("c/x.h")["key"] != base_files.at("cpp/x.h")["key"],
         "the same bytes in two languages are two items");
  Expect(base_files.at("c/moved.h")["language"] == "c" &&
             head_files.at("cpp/moved.h")["language"] == "cpp" &&
             base_files.at("c/moved.h")["key"] !=
                 head_files.at("cpp/moved.h")["key"],
         "a rename across overrides changes the language and key");
  bool renamed = false;
  for (const json& change : moved["changes"]) {
    renamed = renamed || (change["old_path"] == "c/moved.h" &&
                          change["new_path"] == "cpp/moved.h" &&
                          change["status"].get<std::string>().starts_with("R"));
  }
  Expect(renamed, "renames carry both paths");

  // Submodules stay unmeasured entries.
  Git(repo, {"update-index", "--add", "--cacheinfo",
             "160000," + repository.base + ",nested-module"});
  Git(repo, {"commit", "-q", "-m", "submodule"});
  const std::string with_module = Git(repo, {"rev-parse", "HEAD"});
  const json module_plan = llmcc::compare::Prepare(
      Options(repo, with_module, with_module, root / "module"));
  const json module =
      ByPath(module_plan["inventories"]["head"]).at("nested-module");
  Expect(module["reason"] == "submodule" && module["size"].is_null() &&
             module["blob_id"].is_null() && module["scorable"] == false,
         "submodules are recorded without a blob");

  // Empty revisions have nothing to plan.
  const fs::path empty = root / "empty";
  fs::create_directories(empty);
  Git(empty, {"init", "-q"});
  const std::string empty_base = Commit(empty, "empty base");
  const std::string empty_head = Commit(empty, "empty head");
  const json nothing = llmcc::compare::Prepare(
      Options(empty, empty_head, empty_base, root / "nothing"));
  Expect(nothing["workers"].empty() && nothing["items"].empty(),
         "empty revisions plan nothing");

  // Missing history is actionable; bad arguments are refused.
  Expect(Failure<llmcc::git::GitError>([&] {
           static_cast<void>(llmcc::compare::Prepare(
               Options(repo, repository.head, "missing", root / "missing")));
         }).find("fetch") != std::string::npos,
         "unresolvable targets explain how to fix them");
  PrepareOptions extra =
      Options(repo, repository.head, repository.base, root / "extra");
  extra.identity["head_sha"] = "0";
  Expect(!Failure<std::invalid_argument>([&] {
            static_cast<void>(llmcc::compare::Prepare(extra));
          }).empty(),
         "identity files cannot contradict the commits");
  PrepareOptions discovered =
      Options(repo, repository.head, repository.base, root / "discovered");
  discovered.identity["head_sha"] = repository.head;
  discovered.identity["target_sha"] = repository.base;
  discovered.identity["base_sha"] = nullptr;
  Expect(llmcc::compare::Prepare(discovered)["identity"]["base_sha"] ==
             repository.base,
         "a discovered identity names the commits it knows");

  // Committed virtual environments are skipped, as local discovery does.
  const fs::path environment = root / "environment";
  fs::create_directories(environment);
  llmcc::compare::test::Git(environment, {"init", "-q"});
  llmcc::compare::test::Write(environment / "app.py", "x = 1\n");
  llmcc::compare::test::Write(environment / "env/pyvenv.cfg", "home = /usr\n");
  llmcc::compare::test::Write(environment / "env/lib/site.py", "x = 2\n");
  const std::string environment_head =
      llmcc::compare::test::Commit(environment, "environment");
  const auto environment_files = ByPath(llmcc::compare::Prepare(
      Options(environment, environment_head, environment_head,
              root / "environment-plan"))["inventories"]["head"]);
  Expect(environment_files.at("env/lib/site.py")["reason"] == "excluded" &&
             environment_files.at("app.py")["scorable"] == true,
         "files inside a committed virtual environment are excluded");

  // Settings are validated even when no worker will ever read them.
  json unscorable = llmcc::compare::Prepare(
      Options(repo, repository.head, repository.base, root / "settings"));
  unscorable["scoring"]["kv_offload"] = "sometimes";
  Expect(Failure<std::invalid_argument>([&] {
           llmcc::compare::ValidatePlan(unscorable);
         }).find("plan scoring") != std::string::npos,
         "a plan's scoring settings are validated");
  json scorerless = llmcc::compare::Prepare(
      Options(repo, repository.head, repository.base, root / "scorerless"));
  json both = scorerless["scorer"];
  both["commit"] = "abc";
  both["executable"] = std::string(64, 'a');
  for (const json& scorer : std::vector<json>{json::object(), both}) {
    scorerless["scorer"] = scorer;
    scorerless["fingerprint"] = llmcc::compare::Fingerprint(
        scorer, scorerless["model"], scorerless["scoring"]);
    Expect(Failure<std::invalid_argument>([&] {
             llmcc::compare::ValidatePlan(scorerless);
           }).find("plan scorer") != std::string::npos,
           "a plan's scorer identity is validated");
  }
  PrepareOptions huge =
      Options(repo, repository.head, repository.base, root / "huge");
  huge.max_file_bytes = llmcc::kMaxSourceBytes + 1;
  Expect(!Failure<std::invalid_argument>([&] {
            static_cast<void>(llmcc::compare::Prepare(huge));
          }).empty(),
         "files the analyzer refuses are never planned");

#ifndef _WIN32
  // A path that is not UTF-8 is spelled the same, escaped, in the inventory
  // and the changes, and is never scored.
  const fs::path bytes = root / "bytes";
  fs::create_directories(bytes);
  llmcc::compare::test::Git(bytes, {"init", "-q"});
  llmcc::compare::test::Write(bytes / "a.cc", "int a;\n");
  const std::string bytes_base = llmcc::compare::test::Commit(bytes, "base");
  llmcc::compare::test::Write(bytes / fs::path(std::string("bad\xff.py")),
                              "x = 1\n");
  // A valid name spelling the same escape stays a different path.
  llmcc::compare::test::Write(bytes / "bad\\xff.py", "x = 2\n");
  const std::string bytes_head = llmcc::compare::test::Commit(bytes, "head");
  const json escaped = llmcc::compare::Prepare(
      Options(bytes, bytes_head, bytes_base, root / "bytes-plan"));
  const auto escaped_head = ByPath(escaped["inventories"]["head"]);
  Expect(escaped_head.contains("bad\\xff.py") &&
             escaped_head.at("bad\\xff.py")["reason"] == "unsupported",
         "a non-UTF-8 path is escaped and unscored");
  Expect(escaped_head.contains("bad\\\\xff.py") &&
             escaped_head.at("bad\\\\xff.py")["scorable"] == true,
         "a literal backslash is escaped, so the spellings never collide");
  std::set<std::string> changed;
  for (const json& change : escaped["changes"]) {
    changed.insert(change["new_path"].get<std::string>());
  }
  ExpectEq(changed, std::set<std::string>{"bad\\xff.py", "bad\\\\xff.py"},
           "changes spell paths as the inventory does");
#endif
  return 0;
}
