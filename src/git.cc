#include "src/git.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "src/sha1.h"
#include "src/sha256.h"
#include "src/subprocess.h"

namespace llmcc::git {
namespace {

std::string PathArgument(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::u8string value = path.u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
#else
  return path.string();
#endif
}

std::string TrimLine(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
    value.pop_back();
  }
  return value;
}

ProcessResult RunGit(const std::vector<std::string>& arguments,
                     std::optional<std::string> input = {}) {
  std::vector<std::string> argv{"git"};
  argv.insert(argv.end(), arguments.begin(), arguments.end());
  try {
    return RunProcess(argv, {.stdin_data = std::move(input)});
  } catch (const ProcessStartError& error) {
    throw GitError(error.what());
  }
}

std::vector<std::string_view> SplitNul(std::string_view text) {
  std::vector<std::string_view> fields;
  while (!text.empty()) {
    const std::size_t end = text.find('\0');
    fields.push_back(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view{}
                                         : text.substr(end + 1);
  }
  return fields;
}

bool HexObjectId(std::string_view value) {
  return (value.size() == 40 || value.size() == 64) &&
         std::ranges::all_of(value, [](char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

// Revisions come from command lines; one that starts with '-' would be read
// as an option.
void CheckRevision(std::string_view revision) {
  if (revision.empty() || revision.front() == '-') {
    throw GitError("invalid revision '" + std::string(revision) + "'");
  }
}

template <typename Hash>
std::string HashHex(std::string_view header, std::string_view contents) {
  Hash hash;
  hash.Update(std::span<const char>(header.data(), header.size()));
  hash.Update(std::span<const char>(contents.data(), contents.size()));
  const auto digest = hash.Finish();
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2);
  for (const unsigned char byte : digest) {
    result.push_back(kDigits[byte >> 4]);
    result.push_back(kDigits[byte & 0x0f]);
  }
  return result;
}

// Parses one `mode SP type SP id [SP size] TAB path` entry.
TreeEntry ParseTreeEntry(std::string_view entry, bool with_size) {
  const std::size_t tab = entry.find('\t');
  if (tab == std::string_view::npos) {
    throw GitError("malformed git ls-tree entry");
  }
  std::string_view metadata = entry.substr(0, tab);
  std::vector<std::string_view> fields;
  while (!metadata.empty()) {
    const std::size_t space = metadata.find(' ');
    if (space != 0) {
      fields.push_back(metadata.substr(0, space));
    }
    metadata = space == std::string_view::npos ? std::string_view{}
                                               : metadata.substr(space + 1);
  }
  if (fields.size() != (with_size ? 4U : 3U)) {
    throw GitError("malformed git ls-tree entry");
  }
  TreeEntry result{.mode = std::string(fields[0]),
                   .type = std::string(fields[1]),
                   .object_id = std::string(fields[2]),
                   .size = std::nullopt,
                   .path = std::string(entry.substr(tab + 1))};
  if (with_size && fields[3] != "-") {
    std::uint64_t size = 0;
    for (const char digit : fields[3]) {
      if (digit < '0' || digit > '9') {
        throw GitError("malformed git ls-tree size");
      }
      size = size * 10 + static_cast<std::uint64_t>(digit - '0');
    }
    result.size = size;
  }
  return result;
}

}  // namespace

std::string BlobId(std::string_view contents, ObjectFormat format) {
  std::string header = "blob " + std::to_string(contents.size());
  header.push_back('\0');
  return format == ObjectFormat::kSha256 ? HashHex<Sha256>(header, contents)
                                         : HashHex<Sha1>(header, contents);
}

std::optional<std::filesystem::path> FindRepositoryRoot(
    const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path probe =
      std::filesystem::is_directory(path, error) ? path : path.parent_path();
  if (error || probe.empty()) {
    return std::nullopt;
  }
  ProcessResult result;
  try {
    result =
        RunGit({"-C", PathArgument(probe), "rev-parse", "--show-toplevel"});
  } catch (const GitError&) {
    return std::nullopt;
  }
  const std::string root = TrimLine(std::move(result.stdout_data));
  if (result.exit_code != 0 || root.empty()) {
    return std::nullopt;
  }
  const auto canonical =
      std::filesystem::canonical(std::filesystem::u8path(root), error);
  return error ? std::nullopt : std::optional<std::filesystem::path>(canonical);
}

std::optional<std::vector<std::string>> ListFiles(
    const std::filesystem::path& repository, bool include_ignored) {
  std::vector<std::string> arguments{"-C", PathArgument(repository), "ls-files",
                                     "--cached", "--others"};
  if (!include_ignored) {
    arguments.emplace_back("--exclude-standard");
  }
  arguments.emplace_back("-z");
  ProcessResult result;
  try {
    result = RunGit(arguments);
  } catch (const GitError&) {
    return std::nullopt;
  }
  if (result.exit_code != 0) {
    return std::nullopt;
  }
  std::vector<std::string> paths;
  for (const std::string_view path : SplitNul(result.stdout_data)) {
    if (!path.empty()) {
      paths.emplace_back(path);
    }
  }
  return paths;
}

Repository::Repository(std::filesystem::path root) : root_(std::move(root)) {}

std::string Repository::Run(const std::vector<std::string>& arguments,
                            std::optional<std::string> input) const {
  std::vector<std::string> command{"-C", PathArgument(root_)};
  command.insert(command.end(), arguments.begin(), arguments.end());
  ProcessResult result = RunGit(command, std::move(input));
  if (result.exit_code != 0) {
    std::string description;
    for (const std::string& argument : arguments) {
      description += (description.empty() ? "" : " ") + argument;
    }
    throw GitError("git " + description +
                   " failed: " + TrimLine(std::move(result.stderr_data)));
  }
  return std::move(result.stdout_data);
}

std::string Repository::ResolveCommit(std::string_view revision) const {
  CheckRevision(revision);
  std::string value;
  try {
    value = TrimLine(
        Run({"rev-parse", "--verify", std::string(revision) + "^{commit}"}));
  } catch (const GitError& error) {
    throw GitError("cannot resolve revision " + std::string(revision) +
                   " to a commit; fetch the required history: " + error.what());
  }
  if (!HexObjectId(value)) {
    throw GitError("git returned an invalid commit id for " +
                   std::string(revision));
  }
  return value;
}

std::string Repository::MergeBase(std::string_view target,
                                  std::string_view head) const {
  CheckRevision(target);
  CheckRevision(head);
  std::string value;
  try {
    value =
        TrimLine(Run({"merge-base", std::string(target), std::string(head)}));
  } catch (const GitError& error) {
    throw GitError(
        "cannot find merge base for target " + std::string(target) +
        " and head " + std::string(head) +
        "; fetch the target branch and deepen the checkout: " + error.what());
  }
  if (value.empty()) {
    throw GitError(
        "git returned an empty merge base; fetch complete target and head "
        "history");
  }
  return value;
}

ObjectFormat Repository::GetObjectFormat() const {
  const std::string format =
      TrimLine(Run({"rev-parse", "--show-object-format"}));
  if (format == "sha256") {
    return ObjectFormat::kSha256;
  }
  // Git before 2.29 knows only SHA-1 and echoes the unknown option back.
  if (format == "sha1" || format == "--show-object-format") {
    return ObjectFormat::kSha1;
  }
  throw GitError("unsupported Git object format: " + format);
}

std::vector<TreeEntry> Repository::ListTree(std::string_view revision) const {
  CheckRevision(revision);
  const std::string output =
      Run({"ls-tree", "-rlz", "--full-tree", std::string(revision)});
  std::vector<TreeEntry> entries;
  for (const std::string_view entry : SplitNul(output)) {
    if (!entry.empty()) {
      entries.push_back(ParseTreeEntry(entry, true));
    }
  }
  return entries;
}

std::optional<std::string> Repository::ReadFile(std::string_view revision,
                                                std::string_view path) const {
  CheckRevision(revision);
  const std::size_t slash = path.rfind('/');
  const std::string parent(slash == std::string_view::npos
                               ? std::string_view{}
                               : path.substr(0, slash));
  const std::string_view name =
      slash == std::string_view::npos ? path : path.substr(slash + 1);
  // List the parent tree and match the name exactly: ls-tree reads its path
  // arguments as patterns, and a symbolic link is also a blob.
  // A revision Git cannot resolve is a failure, never an absent file.
  static_cast<void>(Run(
      {"rev-parse", "--verify", "--quiet", std::string(revision) + "^{tree}"}));
  const std::string tree =
      std::string(revision) + (parent.empty() ? "^{tree}" : ":" + parent);
  const ProcessResult exists = RunGit(
      {"-C", PathArgument(root_), "rev-parse", "--verify", "--quiet", tree});
  if (exists.exit_code != 0) {
    return std::nullopt;
  }
  const std::string listing =
      Run({"ls-tree", "-z", TrimLine(exists.stdout_data)});
  for (const std::string_view line : SplitNul(listing)) {
    if (line.empty()) {
      continue;
    }
    const TreeEntry entry = ParseTreeEntry(line, false);
    if (entry.path != name) {
      continue;
    }
    if (entry.type != "blob" ||
        (entry.mode != "100644" && entry.mode != "100755")) {
      throw GitError(std::string(path) + " is not a regular file in " +
                     std::string(revision));
    }
    return Run({"cat-file", "blob", entry.object_id});
  }
  return std::nullopt;
}

void Repository::ReadBlobs(
    std::span<const BlobRequest> requests,
    const std::function<void(const std::string&, std::string)>& sink,
    std::uint64_t batch_bytes) const {
  std::size_t start = 0;
  while (start < requests.size()) {
    std::size_t end = start;
    std::uint64_t bytes = 0;
    std::string input;
    while (end < requests.size() &&
           (end == start || bytes + requests[end].size <= batch_bytes)) {
      if (!HexObjectId(requests[end].object_id)) {
        throw GitError("invalid object id " + requests[end].object_id);
      }
      bytes += requests[end].size;
      input += requests[end].object_id + "\n";
      ++end;
    }
    const std::string output = Run({"cat-file", "--batch"}, std::move(input));
    std::string_view remaining = output;
    for (std::size_t index = start; index < end; ++index) {
      const BlobRequest& request = requests[index];
      const std::size_t newline = remaining.find('\n');
      if (newline == std::string_view::npos) {
        throw GitError("truncated git cat-file response");
      }
      const std::string expected =
          request.object_id + " blob " + std::to_string(request.size);
      if (remaining.substr(0, newline) != expected) {
        throw GitError("unexpected git cat-file response for " +
                       request.object_id + ": " +
                       std::string(remaining.substr(0, newline)));
      }
      remaining.remove_prefix(newline + 1);
      if (remaining.size() < request.size + 1 ||
          remaining[request.size] != '\n') {
        throw GitError("malformed git cat-file response");
      }
      sink(request.object_id, std::string(remaining.substr(0, request.size)));
      remaining.remove_prefix(request.size + 1);
    }
    start = end;
  }
}

std::vector<Change> Repository::Diff(std::string_view base,
                                     std::string_view head) const {
  CheckRevision(base);
  CheckRevision(head);
  const std::string output =
      Run({"diff", "--name-status", "-z", "--find-renames", "--no-ext-diff",
           std::string(base), std::string(head)});
  const std::vector<std::string_view> fields = SplitNul(output);
  std::vector<Change> changes;
  std::size_t index = 0;
  const auto next = [&]() -> std::string {
    if (index >= fields.size()) {
      throw GitError("truncated git diff output");
    }
    return std::string(fields[index++]);
  };
  while (index < fields.size() && !fields[index].empty()) {
    Change change{.status = next()};
    const char kind = change.status.front();
    std::string old_path = next();
    std::string new_path =
        kind == 'R' || kind == 'C' ? next() : std::string(old_path);
    if (kind != 'A') {
      change.old_path = std::move(old_path);
    }
    if (kind != 'D') {
      change.new_path = std::move(new_path);
    }
    changes.push_back(std::move(change));
  }
  return changes;
}

}  // namespace llmcc::git
