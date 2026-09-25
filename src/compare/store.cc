#include "src/compare/store.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "src/cache_io.h"
#include "src/compare/json_util.h"
#include "src/compare/s3.h"

namespace llmcc::compare {
namespace {

std::optional<std::string> Environment(const char* name) {
  const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::string(value);
}

std::string PathUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::u8string value = path.u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
#else
  return path.string();
#endif
}

std::unique_ptr<Store> OpenS3(std::string_view rest,
                              const StoreOptions& options) {
  const std::size_t slash = rest.find('/');
  S3Settings settings;
  settings.bucket = std::string(rest.substr(0, slash));
  if (settings.bucket.empty()) {
    throw StoreError("S3 store location needs a bucket");
  }
  // The bucket is a URL path segment for custom endpoints, which libcurl
  // would squash if it were "." or "..".
  try {
    ValidateStoreKey(settings.bucket);
  } catch (const StoreError&) {
    throw StoreError("invalid S3 bucket '" + settings.bucket + "'");
  }
  if (slash != std::string_view::npos) {
    std::string_view prefix = rest.substr(slash + 1);
    while (!prefix.empty() && prefix.front() == '/') {
      prefix.remove_prefix(1);
    }
    while (!prefix.empty() && prefix.back() == '/') {
      prefix.remove_suffix(1);
    }
    // libcurl squashes "." and ".." segments, which would address other
    // objects than the configured prefix names.
    if (!prefix.empty()) {
      try {
        ValidateStoreKey(prefix);
      } catch (const StoreError&) {
        throw StoreError("invalid S3 store prefix '" + std::string(prefix) +
                         "'");
      }
    }
    settings.prefix = std::string(prefix);
  }
  settings.endpoint = options.endpoint_url;
  if (!settings.endpoint.has_value()) {
    settings.endpoint = Environment("AWS_ENDPOINT_URL_S3");
  }
  if (!settings.endpoint.has_value()) {
    settings.endpoint = Environment("AWS_ENDPOINT_URL");
  }
  settings.region = options.region_name.value_or(
      Environment("AWS_REGION")
          .value_or(Environment("AWS_DEFAULT_REGION").value_or("us-east-1")));
  const auto access_key_id = Environment("AWS_ACCESS_KEY_ID");
  const auto secret_access_key = Environment("AWS_SECRET_ACCESS_KEY");
  if (!access_key_id.has_value() || !secret_access_key.has_value()) {
    throw StoreError(
        "S3 store needs AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY in the "
        "environment");
  }
  settings.access_key_id = *access_key_id;
  settings.secret_access_key = *secret_access_key;
  settings.session_token = Environment("AWS_SESSION_TOKEN");
  return std::make_unique<S3Store>(std::move(settings), MakeCurlTransport());
}

}  // namespace

void ValidateStoreKey(std::string_view key) {
  if (key.empty() || key.front() == '/') {
    throw StoreError("invalid store key '" + std::string(key) + "'");
  }
  std::string_view remaining = key;
  while (true) {
    const std::size_t end = remaining.find('/');
    const std::string_view segment = remaining.substr(0, end);
    // A colon names a drive or an alternate data stream on Windows, where
    // `root / key` would then leave the store.
    if (segment.empty() || segment == "." || segment == ".." ||
        segment.find_first_of(std::string_view("\\:\0", 3)) !=
            std::string_view::npos) {
      throw StoreError("invalid store key '" + std::string(key) + "'");
    }
    if (end == std::string_view::npos) {
      return;
    }
    remaining.remove_prefix(end + 1);
  }
}

namespace {

// Other writers of a shared store could swap a key directory for a symlink
// and redirect reads or writes outside the root.
#ifdef _WIN32
// Every directory between the root and the entry must be a real one. With
// `create`, missing directories are made one level at a time and checked in
// turn. Returns false for a missing directory, which a read treats as a miss.
bool CheckedParents(const std::filesystem::path& root, std::string_view key,
                    bool create) {
  std::filesystem::path current = root;
  std::string_view remaining = key;
  for (std::size_t end = remaining.find('/'); end != std::string_view::npos;
       end = remaining.find('/')) {
    current /= std::filesystem::u8path(remaining.substr(0, end));
    remaining.remove_prefix(end + 1);
    std::error_code error;
    auto status = std::filesystem::symlink_status(current, error);
    if (status.type() == std::filesystem::file_type::not_found && create) {
      std::filesystem::create_directory(current, error);
      status = std::filesystem::symlink_status(current, error);
    }
    if (status.type() == std::filesystem::file_type::not_found) {
      return false;
    }
    if (error || status.type() != std::filesystem::file_type::directory) {
      throw StoreError("store entry " + std::string(key) +
                       " passes through a symlink or something other than a "
                       "directory");
    }
  }
  return true;
}
#else
// Each directory is opened relative to the previous one without following
// symlinks, and the entry is read or replaced through the last directory's
// descriptor, so no later path lookup can be redirected.
class Descriptor {
 public:
  explicit Descriptor(int descriptor = -1) : descriptor_(descriptor) {}
  Descriptor(Descriptor&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}
  Descriptor& operator=(Descriptor&& other) noexcept {
    if (this != &other) {
      Reset();
      descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
  }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  ~Descriptor() { Reset(); }
  [[nodiscard]] int get() const { return descriptor_; }

 private:
  void Reset() {
    if (descriptor_ >= 0) {
      close(descriptor_);
      descriptor_ = -1;
    }
  }
  int descriptor_;
};

[[noreturn]] void Fail(std::string_view action, std::string_view key,
                       int error) {
  throw StoreError(std::string(action) + " store entry " + std::string(key) +
                   ": " +
                   (error == ELOOP || error == ENOTDIR
                        ? std::string("it passes through a symlink or "
                                      "something other than a directory")
                        : std::generic_category().message(error)));
}

// The directory holding `key`'s entry, whose name goes to `name`. With
// `create`, missing directories are made one level at a time. An invalid
// descriptor means a directory is missing, which a read treats as a miss.
Descriptor OpenParent(const std::filesystem::path& root, std::string_view key,
                      bool create, std::string& name) {
  const std::string_view action = create ? "cannot write" : "cannot read";
  if (create) {
    std::filesystem::create_directories(root);
  }
  Descriptor current(open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (current.get() < 0) {
    if (errno == ENOENT) {
      return Descriptor();
    }
    Fail(action, key, errno);
  }
  constexpr int kDirectory = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
  std::string_view remaining = key;
  for (std::size_t end = remaining.find('/'); end != std::string_view::npos;
       end = remaining.find('/')) {
    const std::string segment(remaining.substr(0, end));
    remaining.remove_prefix(end + 1);
    int next = openat(current.get(), segment.c_str(), kDirectory);
    if (next < 0 && errno == ENOENT && create) {
      if (mkdirat(current.get(), segment.c_str(), 0777) != 0 &&
          errno != EEXIST) {
        Fail(action, key, errno);
      }
      next = openat(current.get(), segment.c_str(), kDirectory);
    }
    if (next < 0) {
      if (errno == ENOENT) {
        return Descriptor();
      }
      Fail(action, key, errno);
    }
    current = Descriptor(next);
  }
  name = std::string(remaining);
  return current;
}
#endif

}  // namespace

FilesystemStore::FilesystemStore(std::filesystem::path root)
    : root_(std::move(root)) {}

std::filesystem::path FilesystemStore::PathOf(std::string_view key) const {
  ValidateStoreKey(key);
  return root_ / std::filesystem::u8path(key);
}

std::optional<std::string> FilesystemStore::Get(std::string_view key) {
#ifdef _WIN32
  const std::filesystem::path path = PathOf(key);
  if (!CheckedParents(root_, key, false)) {
    return std::nullopt;
  }
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory ||
      status.type() == std::filesystem::file_type::not_found) {
    return std::nullopt;
  }
  if (status.type() == std::filesystem::file_type::symlink) {
    throw StoreError("cannot read store entry " + std::string(key) +
                     ": it is a symlink");
  }
  if (error || status.type() != std::filesystem::file_type::regular) {
    throw StoreError("cannot read store entry " + std::string(key) + ": " +
                     (error ? error.message() : "not a regular file"));
  }
  if (std::filesystem::file_size(path, error) > kMaxStoreObjectBytes &&
      !error) {
    throw StoreError("store entry " + std::string(key) + " exceeds " +
                     std::to_string(kMaxStoreObjectBytes) + " bytes");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    throw StoreError("cannot open store entry " + std::string(key));
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  if (input.bad()) {
    throw StoreError("cannot read store entry " + std::string(key));
  }
  return contents.str();
#else
  ValidateStoreKey(key);
  std::string name;
  const Descriptor parent = OpenParent(root_, key, false, name);
  if (parent.get() < 0) {
    return std::nullopt;
  }
  // O_NONBLOCK keeps a planted FIFO from blocking the open.
  const Descriptor file(openat(parent.get(), name.c_str(),
                               O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  if (file.get() < 0) {
    if (errno == ENOENT) {
      return std::nullopt;
    }
    if (errno == ELOOP) {
      throw StoreError("cannot read store entry " + std::string(key) +
                       ": it is a symlink");
    }
    Fail("cannot read", key, errno);
  }
  struct stat information{};
  if (fstat(file.get(), &information) != 0 || !S_ISREG(information.st_mode)) {
    throw StoreError("cannot read store entry " + std::string(key) +
                     ": not a regular file");
  }
  // Checked before and while reading, since another writer can still grow
  // the file.
  const auto too_large = [&] {
    return StoreError("store entry " + std::string(key) + " exceeds " +
                      std::to_string(kMaxStoreObjectBytes) + " bytes");
  };
  if (std::cmp_greater(information.st_size, kMaxStoreObjectBytes)) {
    throw too_large();
  }
  std::string contents;
  std::array<char, std::size_t{64} * 1024> buffer{};
  while (true) {
    const ssize_t count = read(file.get(), buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      Fail("cannot read", key, errno);
    }
    if (count == 0) {
      return contents;
    }
    if (contents.size() + static_cast<std::size_t>(count) >
        kMaxStoreObjectBytes) {
      throw too_large();
    }
    contents.append(buffer.data(), static_cast<std::size_t>(count));
  }
#endif
}

void FilesystemStore::Put(std::string_view key, std::string_view value) {
#ifdef _WIN32
  const std::filesystem::path path = PathOf(key);
  try {
    std::filesystem::create_directories(root_);
    CheckedParents(root_, key, true);
    cache_io::AtomicWriteFile(path, value, 0660);
  } catch (const StoreError&) {
    throw;
  } catch (const std::exception& error) {
    throw StoreError("cannot write store entry " + std::string(key) + ": " +
                     error.what());
  }
#else
  ValidateStoreKey(key);
  std::string name;
  Descriptor parent;
  try {
    parent = OpenParent(root_, key, true, name);
  } catch (const std::filesystem::filesystem_error& error) {
    throw StoreError("cannot write store entry " + std::string(key) + ": " +
                     error.what());
  }
  struct stat existing{};
  if (fstatat(parent.get(), name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) ==
          0 &&
      S_ISLNK(existing.st_mode)) {
    throw StoreError("cannot write store entry " + std::string(key) +
                     ": it is a symlink");
  }
  // Honor the deployment's umask so a provisioned group can read and replace
  // objects that another executor account wrote.
  std::string temporary;
  Descriptor file;
  for (int attempt = 0; attempt != 10 && file.get() < 0; ++attempt) {
    temporary = "." + name + cache_io::UniqueSuffix();
    file = Descriptor(
        openat(parent.get(), temporary.c_str(),
               O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0660));
    if (file.get() < 0 && errno != EEXIST) {
      Fail("cannot write", key, errno);
    }
  }
  if (file.get() < 0) {
    Fail("cannot write", key, EEXIST);
  }
  const auto abandon = [&](int error) {
    unlinkat(parent.get(), temporary.c_str(), 0);
    Fail("cannot write", key, error);
  };
  for (std::size_t written = 0; written < value.size();) {
    const ssize_t count =
        write(file.get(), value.data() + written, value.size() - written);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      abandon(errno);
    }
    written += static_cast<std::size_t>(count);
  }
  if (fsync(file.get()) != 0) {
    abandon(errno);
  }
  file = Descriptor();
  if (renameat(parent.get(), temporary.c_str(), parent.get(), name.c_str()) !=
      0) {
    abandon(errno);
  }
  // A crash between rename and a later power loss must not lose the name;
  // the data itself was synced before the rename.
  static_cast<void>(fsync(parent.get()));
#endif
}

std::string FilesystemStore::Describe() const { return PathUtf8(root_); }

StoreOptions ParseStoreOptions(const nlohmann::json& options) {
  if (!options.is_object()) {
    throw StoreError("store options must be a JSON object");
  }
  StoreOptions parsed;
  for (const auto& [name, value] : options.items()) {
    if (value.is_null()) {
      continue;
    }
    if (name == "endpoint_url" && value.is_string()) {
      parsed.endpoint_url = value.get<std::string>();
    } else if (name == "region_name" && value.is_string()) {
      parsed.region_name = value.get<std::string>();
    } else {
      throw StoreError("unsupported store option " + name +
                       "; only endpoint_url and region_name are read, and "
                       "credentials come from the environment");
    }
  }
  return parsed;
}

StoreOptions ReadStoreOptions(
    const std::optional<std::filesystem::path>& path) {
  if (!path.has_value()) {
    return {};
  }
  try {
    return ParseStoreOptions(ReadJsonFile(*path));
  } catch (const StoreError&) {
    throw;
  } catch (const std::exception& error) {
    throw StoreError(std::string("cannot read store options: ") + error.what());
  }
}

std::unique_ptr<Store> OpenStore(std::string_view location,
                                 const StoreOptions& options) {
  if (location.starts_with("s3://")) {
    return OpenS3(location.substr(5), options);
  }
  if (location.empty() || location.find("://") != std::string_view::npos) {
    throw StoreError("unsupported store location: " + std::string(location));
  }
  return std::make_unique<FilesystemStore>(std::filesystem::u8path(location));
}

}  // namespace llmcc::compare
