#ifndef LLM_CC_GIT_H_
#define LLM_CC_GIT_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace llmcc::git {

class GitError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

enum class ObjectFormat : std::uint8_t { kSha1, kSha256 };

// The Git object name of a blob with these contents.
std::string BlobId(std::string_view contents, ObjectFormat format);

// The worktree root containing `path`, or nullopt when there is none or Git
// is unavailable.
std::optional<std::filesystem::path> FindRepositoryRoot(
    const std::filesystem::path& path);

// Tracked and untracked paths, '/'-separated and relative to the root, as
// `git ls-files` spells them. Without `include_ignored`, .gitignore applies.
// Returns nullopt when Git fails.
std::optional<std::vector<std::string>> ListFiles(
    const std::filesystem::path& repository, bool include_ignored);

struct TreeEntry {
  std::string mode;
  std::string type;
  std::string object_id;
  // Blob size; nullopt for submodules and other non-blob entries.
  std::optional<std::uint64_t> size;
  std::string path;
};

struct Change {
  // Git's status letter, with a similarity score for renames and copies.
  std::string status;
  std::optional<std::string> old_path;
  std::optional<std::string> new_path;
};

struct BlobRequest {
  std::string object_id;
  std::uint64_t size = 0;
};

// Committed objects of one repository. Every method runs Git without a
// shell and throws GitError with Git's own diagnostic on failure.
class Repository {
 public:
  explicit Repository(std::filesystem::path root);

  [[nodiscard]] const std::filesystem::path& root() const { return root_; }
  [[nodiscard]] std::string ResolveCommit(std::string_view revision) const;
  [[nodiscard]] std::string MergeBase(std::string_view target,
                                      std::string_view head) const;
  [[nodiscard]] ObjectFormat GetObjectFormat() const;
  // Every entry of a commit's tree, recursively.
  [[nodiscard]] std::vector<TreeEntry> ListTree(
      std::string_view revision) const;
  // One committed file, or nullopt when the tree has no such path. Throws
  // when the path names something other than a regular blob.
  [[nodiscard]] std::optional<std::string> ReadFile(
      std::string_view revision, std::string_view path) const;
  // Streams blob contents to `sink` in request order, reading them with one
  // `git cat-file --batch` per batch of at most `batch_bytes`.
  void ReadBlobs(std::span<const BlobRequest> requests,
                 const std::function<void(const std::string& object_id,
                                          std::string contents)>& sink,
                 std::uint64_t batch_bytes = std::uint64_t{64} * 1024 *
                                             1024) const;
  // Name-status changes from `base` to `head`, with rename detection.
  [[nodiscard]] std::vector<Change> Diff(std::string_view base,
                                         std::string_view head) const;

 private:
  [[nodiscard]] std::string Run(const std::vector<std::string>& arguments,
                                std::optional<std::string> input = {}) const;
  std::filesystem::path root_;
};

}  // namespace llmcc::git

#endif  // LLM_CC_GIT_H_
