#ifndef LLM_CC_COMPARE_STORE_H_
#define LLM_CC_COMPARE_STORE_H_

#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace llmcc::compare {

// The store could not be read or written, as opposed to an absent object.
class StoreError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// A flat object store of '/'-separated keys. Writes replace whole objects
// atomically and the last writer wins; there is no listing and no
// conditional write.
class Store {
 public:
  Store() = default;
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  Store(Store&&) = delete;
  Store& operator=(Store&&) = delete;
  virtual ~Store() = default;

  // The object's bytes, or nullopt when it does not exist. Throws StoreError
  // for any other failure, so a broken store never reads as a cache miss.
  virtual std::optional<std::string> Get(std::string_view key) = 0;
  virtual void Put(std::string_view key, std::string_view value) = 0;
  // Where the store keeps objects, for messages.
  [[nodiscard]] virtual std::string Describe() const = 0;
};

// Rejects keys that are empty, rooted, or contain empty, "." or ".."
// segments, which no store may resolve outside its root.
void ValidateStoreKey(std::string_view key);

// Objects as files under a root directory. New files are group-writable
// (0660 before the umask), so executors sharing a group can share a store.
// On POSIX every access goes through no-follow directory descriptors, so
// another writer cannot redirect it outside the root. On Windows symlinks
// and junctions are refused by checking each directory, which a concurrent
// writer could still race; share a Windows store only with trusted writers.
class FilesystemStore : public Store {
 public:
  explicit FilesystemStore(std::filesystem::path root);
  std::optional<std::string> Get(std::string_view key) override;
  void Put(std::string_view key, std::string_view value) override;
  [[nodiscard]] std::string Describe() const override;

 private:
  [[nodiscard]] std::filesystem::path PathOf(std::string_view key) const;
  std::filesystem::path root_;
};

// S3 client settings from a --store-options file, named as boto3 names
// them. Credentials never come from here, only from the environment.
struct StoreOptions {
  std::optional<std::string> endpoint_url;
  std::optional<std::string> region_name;
};

StoreOptions ParseStoreOptions(const nlohmann::json& options);
StoreOptions ReadStoreOptions(const std::optional<std::filesystem::path>& path);

// A filesystem directory, or s3://bucket[/prefix]. S3 credentials come from
// AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY and AWS_SESSION_TOKEN; the endpoint
// and region from `options`, else AWS_ENDPOINT_URL_S3, AWS_ENDPOINT_URL,
// AWS_REGION and AWS_DEFAULT_REGION.
std::unique_ptr<Store> OpenStore(std::string_view location,
                                 const StoreOptions& options = {});

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_STORE_H_
