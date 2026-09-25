#include "src/git.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "src/subprocess.h"
#include "src/test_util.h"

namespace {

namespace fs = std::filesystem;
using llmcc::git::ObjectFormat;
using llmcc::test::Expect;
using llmcc::test::ExpectEq;

std::string Utf8(const fs::path& path) {
  const std::u8string value = path.u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

// Runs git in `repository` and returns its trimmed standard output.
std::string Git(const fs::path& repository,
                const std::vector<std::string>& arguments,
                const std::string& input = {}) {
  std::vector<std::string> command = {
      "git",
      "-C",
      Utf8(repository),
      "-c",
      "user.name=Fixture",
      "-c",
      "user.email=fixture@example.invalid",
      "-c",
      "commit.gpgsign=false",
      "-c",
      "core.autocrlf=false",
  };
  command.insert(command.end(), arguments.begin(), arguments.end());
  auto result = llmcc::RunProcess(
      command, input.empty() ? llmcc::ProcessOptions{}
                             : llmcc::ProcessOptions{.stdin_data = input});
  if (result.exit_code != 0) {
    std::string description;
    for (const std::string& argument : arguments) {
      description += argument + " ";
    }
    Expect(false, "git " + description + "failed: " + result.stderr_data);
  }
  while (!result.stdout_data.empty() && (result.stdout_data.back() == '\n' ||
                                         result.stdout_data.back() == '\r')) {
    result.stdout_data.pop_back();
  }
  return result.stdout_data;
}

void Write(const fs::path& path, std::string_view value) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << value;
}

bool Throws(const auto& function, std::string_view fragment) {
  try {
    function();
  } catch (const llmcc::git::GitError& error) {
    return std::string(error.what()).find(fragment) != std::string::npos;
  }
  return false;
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  const char* temporary = std::getenv("TEST_TMPDIR");
  Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const fs::path root = fs::path(temporary) / "git repository";
  fs::create_directories(root);
  Git(root, {"init", "-q"});
  const std::string unicode_path =
      "dir with space/za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87.py";
  Write(root / "a.cc", "int a;\n");
  Write(root / "move.cc", "int move_me_somewhere_else = 1;\n");
  Write(root / "gone.go", "package gone\n");
  Write(root / fs::u8path(unicode_path), "x = 1\n");
  Write(root / ".llm-cc/rules.json", R"({"tests": ["tests/**"]})");
  Write(root / ".gitignore", "ignored.txt\n");
  Git(root, {"add", "."});
  // Symbolic links and submodules are index entries, so the fixture needs no
  // filesystem symlink support.
  const std::string link_target =
      Git(root, {"hash-object", "-w", "--stdin"}, "a.cc");
  Git(root, {"update-index", "--add", "--cacheinfo",
             "120000," + link_target + ",link.cc"});
  Git(root, {"commit", "-qm", "base"});
  const std::string base = Git(root, {"rev-parse", "HEAD"});
  Write(root / "a.cc", "int a = 2;\n");
  Write(root / "new.py", "def new(): pass\n");
  Git(root, {"rm", "-q", "gone.go"});
  fs::create_directories(root / "tests");
  Git(root, {"mv", "move.cc", "tests/move.cc"});
  // Stage by name: `git add .` would drop the index-only entries.
  Git(root, {"add", "a.cc", "new.py"});
  Git(root,
      {"update-index", "--add", "--cacheinfo", "160000," + base + ",module"});
  Git(root, {"commit", "-qm", "head"});
  const std::string head = Git(root, {"rev-parse", "HEAD"});
  Write(root / "ignored.txt", "ignored\n");
  Write(root / "untracked.rs", "fn untracked() {}\n");

  // Worktree discovery.
  ExpectEq(llmcc::git::FindRepositoryRoot(root / "dir with space"),
           std::optional(fs::canonical(root)),
           "a subdirectory finds its worktree root");
  ExpectEq(llmcc::git::FindRepositoryRoot(root / "a.cc"),
           std::optional(fs::canonical(root)), "a file finds its worktree");
  const fs::path outside = fs::path(temporary) / "outside";
  fs::create_directories(outside);
  Expect(!llmcc::git::FindRepositoryRoot(outside).has_value(),
         "a directory outside Git has no worktree");
  const auto listed = llmcc::git::ListFiles(root, false);
  Expect(listed.has_value() &&
             std::ranges::count(*listed, "untracked.rs") == 1 &&
             std::ranges::count(*listed, "ignored.txt") == 0 &&
             std::ranges::count(*listed, unicode_path) == 1,
         "listing includes untracked files and honors .gitignore");
  const auto everything = llmcc::git::ListFiles(root, true);
  Expect(everything.has_value() &&
             std::ranges::count(*everything, "ignored.txt") == 1,
         "listing can include ignored files");
  Expect(!llmcc::git::ListFiles(outside, false).has_value(),
         "listing outside Git fails softly");

  // Revisions.
  const llmcc::git::Repository repository(root);
  ExpectEq(repository.ResolveCommit("HEAD"), head, "HEAD resolves");
  ExpectEq(repository.ResolveCommit(base.substr(0, 12)), base,
           "abbreviated commits resolve");
  Expect(Throws([&] { static_cast<void>(repository.ResolveCommit("missing")); },
                "fetch the required history"),
         "an unknown revision is actionable");
  Expect(Throws([&] { static_cast<void>(repository.ResolveCommit("--all")); },
                "invalid revision"),
         "a revision cannot inject an option");
  ExpectEq(repository.MergeBase(base, head), base, "merge base resolves");
  Expect(
      Throws([&] { static_cast<void>(repository.MergeBase("missing", head)); },
             "fetch the target branch and deepen the checkout"),
      "missing history is actionable");
  ExpectEq(repository.GetObjectFormat(), ObjectFormat::kSha1,
           "a default repository uses SHA-1");
  Expect(Throws(
             [&] {
               static_cast<void>(
                   llmcc::git::Repository(fs::path(temporary) / "missing")
                       .GetObjectFormat());
             },
             "rev-parse --show-object-format failed"),
         "a repository Git cannot open fails object-format detection");

  // Trees keep symbolic links and submodules as unmeasured entries.
  const auto tree = repository.ListTree(head);
  std::map<std::string, llmcc::git::TreeEntry> entries;
  for (const auto& entry : tree) {
    entries.emplace(entry.path, entry);
  }
  Expect(entries.contains("tests/move.cc") && !entries.contains("move.cc") &&
             !entries.contains("gone.go") && entries.contains(unicode_path),
         "the head tree lists every committed path");
  Expect(entries.at("link.cc").mode == "120000" &&
             entries.at("link.cc").type == "blob",
         "a symbolic link is a blob with its own mode");
  Expect(entries.at("module").mode == "160000" &&
             entries.at("module").type == "commit" &&
             !entries.at("module").size.has_value(),
         "a submodule has no blob size");
  ExpectEq(entries.at("a.cc").size, std::optional<std::uint64_t>(11),
           "blob sizes are reported");

  // Blob contents stream in request order across batches, and every blob's
  // name is its content hash.
  std::vector<llmcc::git::BlobRequest> requests;
  for (const std::string path : {"a.cc", "new.py", "tests/move.cc"}) {
    requests.push_back({.object_id = entries.at(path).object_id,
                        .size = *entries.at(path).size});
  }
  std::vector<std::string> contents;
  repository.ReadBlobs(
      requests,
      [&](const std::string& object_id, std::string blob) {
        ExpectEq(llmcc::git::BlobId(blob, ObjectFormat::kSha1), object_id,
                 "blob contents hash to their object id");
        contents.push_back(std::move(blob));
      },
      1);
  ExpectEq(contents,
           std::vector<std::string>{"int a = 2;\n", "def new(): pass\n",
                                    "int move_me_somewhere_else = 1;\n"},
           "blob contents arrive in request order");
  for (const std::string& sample :
       {std::string(), std::string("x\0y", 3), std::string(70000, 'z')}) {
    ExpectEq(
        llmcc::git::BlobId(sample, ObjectFormat::kSha1),
        Git(root, {"hash-object", "--stdin"}, sample.empty() ? "" : sample),
        "BlobId matches git hash-object");
  }

  // Committed files.
  ExpectEq(repository.ReadFile(base, ".llm-cc/rules.json"),
           std::optional<std::string>(R"({"tests": ["tests/**"]})"),
           "a committed file is read");
  try {
    static_cast<void>(repository.ReadFile(std::string(40, 'f'), "a"));
    Expect(false, "an unresolvable revision is an error");
  } catch (const llmcc::git::GitError&) {  // NOLINT(bugprone-empty-catch)
  }
  Expect(
      !repository.ReadFile(base, ".llm-cc/comparison-rules.json").has_value() &&
          !repository.ReadFile(base, "absent/dir/file").has_value(),
      "a missing path reads as nullopt");
  Expect(
      Throws([&] { static_cast<void>(repository.ReadFile(head, "link.cc")); },
             "is not a regular file"),
      "a symbolic link is not read as a file");
  Expect(
      Throws([&] { static_cast<void>(repository.ReadFile(head, ".llm-cc")); },
             "is not a regular file"),
      "a directory is not read as a file");

  // Name-status changes with rename detection.
  std::map<std::string, llmcc::git::Change> changes;
  for (const auto& change : repository.Diff(base, head)) {
    changes.emplace(change.new_path.value_or(*change.old_path), change);
  }
  Expect(changes.at("tests/move.cc").status.starts_with("R") &&
             changes.at("tests/move.cc").old_path == "move.cc",
         "renames carry both paths");
  Expect(changes.at("new.py").status == "A" &&
             !changes.at("new.py").old_path.has_value(),
         "additions have no old path");
  Expect(changes.at("gone.go").status == "D" &&
             !changes.at("gone.go").new_path.has_value(),
         "deletions have no new path");
  Expect(
      changes.at("a.cc").status == "M" && changes.at("a.cc").old_path == "a.cc",
      "modifications keep their path");

  // SHA-256 repositories name blobs with SHA-256 where Git supports them.
  const fs::path sha256_root = fs::path(temporary) / "sha256";
  fs::create_directories(sha256_root);
  const auto created = llmcc::RunProcess(
      {"git", "-C", Utf8(sha256_root), "init", "-q", "--object-format=sha256"});
  if (created.exit_code == 0) {
    Write(sha256_root / "a.rs", "fn a() {}\n");
    Git(sha256_root, {"add", "."});
    Git(sha256_root, {"commit", "-qm", "sha256"});
    const llmcc::git::Repository modern(sha256_root);
    ExpectEq(modern.GetObjectFormat(), ObjectFormat::kSha256,
             "SHA-256 repositories are recognized");
    const auto modern_tree = modern.ListTree("HEAD");
    Expect(modern_tree.size() == 1 && modern_tree[0].object_id.size() == 64,
           "SHA-256 trees name 64-digit objects");
    ExpectEq(llmcc::git::BlobId("fn a() {}\n", ObjectFormat::kSha256),
             modern_tree[0].object_id, "SHA-256 blob ids match Git");
    ExpectEq(modern.ResolveCommit("HEAD").size(), std::size_t{64},
             "SHA-256 commits resolve");
  }
  return 0;
}
