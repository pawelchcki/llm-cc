#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "src/compare/s3.h"
#include "src/compare/store.h"
#include "src/sha256.h"
#include "src/test_util.h"

namespace {

namespace fs = std::filesystem;
using llmcc::compare::HttpRequest;
using llmcc::compare::HttpResponse;
using llmcc::compare::S3Settings;
using llmcc::compare::S3Store;
using llmcc::compare::StoreError;
using llmcc::test::Expect;
using llmcc::test::ExpectEq;

// Replays scripted responses and records every request.
class FakeTransport : public llmcc::compare::HttpTransport {
 public:
  struct Step {
    std::optional<HttpResponse> response;  // nullopt: a transport failure
  };
  explicit FakeTransport(std::vector<HttpRequest>& requests)
      : requests_(requests) {}
  void Then(long status, std::string body = {}) {
    steps_.push_back({HttpResponse{.status = status, .body = std::move(body)}});
  }
  void ThenFail() { steps_.push_back({std::nullopt}); }
  HttpResponse Perform(const HttpRequest& request) override {
    requests_.push_back(request);
    Expect(!steps_.empty(), "every request has a scripted response");
    Step step = std::move(steps_.front());
    steps_.pop_front();
    if (!step.response.has_value()) {
      throw llmcc::compare::TransportError("connection reset");
    }
    return *step.response;
  }

 private:
  std::vector<HttpRequest>& requests_;
  std::deque<Step> steps_;
};

struct S3Fixture {
  std::vector<HttpRequest> requests;
  std::vector<std::chrono::milliseconds> sleeps;
  FakeTransport* transport = nullptr;
  std::unique_ptr<S3Store> store;

  explicit S3Fixture(S3Settings settings = {
                         .bucket = "bucket",
                         .prefix = "llm-cc",
                         .endpoint = "http://127.0.0.1:9000/",
                         .region = "eu-west-1",
                         .access_key_id = "AKID",
                         .secret_access_key = "secret"}) {
    auto owned = std::make_unique<FakeTransport>(requests);
    transport = owned.get();
    store = std::make_unique<S3Store>(
        std::move(settings), std::move(owned),
        [this](std::chrono::milliseconds delay) { sleeps.push_back(delay); });
  }
};

bool HasHeader(const HttpRequest& request, const std::string& header) {
  return std::ranges::find(request.headers, header) != request.headers.end();
}

void WriteText(const fs::path& path, std::string_view text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

template <typename Function>
std::string StoreFailure(Function function) {
  try {
    function();
  } catch (const StoreError& error) {
    return error.what();
  }
  return {};
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  const char* temporary = std::getenv("TEST_TMPDIR");
  Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const fs::path root = fs::path(temporary) / "store";

  // Keys cannot leave the store.
  for (const char* key :
       {"", "/rooted", "a//b", "a/./b", "a/../b", "..", "trailing/",
        "back\\slash", "C:/outside", "D:relative", "file.json:stream"}) {
    Expect(
        !StoreFailure([&] { llmcc::compare::ValidateStoreKey(key); }).empty(),
        std::string("invalid key is rejected: ") + key);
  }
  llmcc::compare::ValidateStoreKey("results/v2/abc.json");

  // Filesystem objects round-trip and absent objects are misses.
  llmcc::compare::FilesystemStore filesystem(root);
  Expect(!filesystem.Get("results/v2/missing.json").has_value(),
         "an absent object is a miss");
  filesystem.Put("results/v2/a.json", "first");
  filesystem.Put("results/v2/a.json", std::string("second\0bytes", 12));
  ExpectEq(filesystem.Get("results/v2/a.json"),
           std::optional<std::string>(std::string("second\0bytes", 12)),
           "the last write wins, byte for byte");
  Expect(std::ranges::none_of(fs::directory_iterator(root / "results/v2"),
                              [](const auto& entry) {
                                return entry.path().filename().string().find(
                                           ".tmp.") != std::string::npos;
                              }),
         "atomic writes leave no temporary files");
  fs::create_directories(root / "results/v2/directory.json");
  Expect(!StoreFailure([&] {
            filesystem.Get("results/v2/directory.json");
          }).empty(),
         "an unreadable object is an error, not a miss");

#if !defined(_WIN32)
  // Entries never resolve through a symlink out of the store.
  const fs::path outside = fs::path(temporary) / "outside";
  fs::create_directories(outside);
  WriteText(outside / "x.json", "outside");
  fs::create_symlink(outside, root / "linked");
  WriteText(root / "results/v2/entry.json", "inside");
  fs::create_symlink(root / "results/v2/entry.json", root / "alias.json");
  for (const char* key : {"linked/x.json", "alias.json"}) {
    Expect(StoreFailure([&] { filesystem.Get(key); }).find("symlink") !=
               std::string::npos,
           std::string("a read through a symlink is refused: ") + key);
  }
  Expect(!StoreFailure([&] {
            filesystem.Put("linked/y.json", "escaped");
          }).empty() &&
             !fs::exists(outside / "y.json"),
         "a write through a symlinked directory is refused");

  // Concurrent writers of one key always leave one complete value.
  std::vector<std::thread> writers;
  for (int writer = 0; writer < 8; ++writer) {
    writers.emplace_back([&, writer] {
      for (int round = 0; round < 20; ++round) {
        filesystem.Put("contended",
                       std::string(4096, static_cast<char>('a' + writer)));
      }
    });
  }
  for (std::thread& writer : writers) {
    writer.join();
  }
  const auto contended = filesystem.Get("contended");
  Expect(contended.has_value() && contended->size() == 4096 &&
             std::ranges::all_of(
                 *contended, [&](char c) { return c == contended->front(); }),
         "concurrent writes never interleave");

  // Shared stores are group-writable within the umask.
  const fs::path shared = fs::path(temporary) / "shared";
  fs::create_directories(shared);
  fs::permissions(
      shared, fs::perms::owner_all | fs::perms::group_all | fs::perms::set_gid);
  llmcc::compare::FilesystemStore group(shared);
  for (const auto& [mask, mode] :
       {std::pair{0007U, 0660U}, std::pair{0077U, 0600U},
        std::pair{0022U, 0640U}}) {
    const mode_t previous = umask(static_cast<mode_t>(mask));
    group.Put("pipelines/plan.json", "first");
    group.Put("pipelines/plan.json", "replaced");
    umask(previous);
    struct stat details{};
    Expect(stat((shared / "pipelines/plan.json").c_str(), &details) == 0,
           "the object exists");
    ExpectEq(details.st_mode & 0777U, mode, "the umask shapes the file mode");
    struct stat directory{};
    stat(shared.c_str(), &directory);
    ExpectEq(details.st_gid, directory.st_gid,
             "objects inherit the shared group");
  }
#endif

  // Store locations.
  Expect(dynamic_cast<llmcc::compare::FilesystemStore*>(
             llmcc::compare::OpenStore(root.string()).get()) != nullptr,
         "a path opens a filesystem store");
  Expect(
      !StoreFailure([] { llmcc::compare::OpenStore("gs://bucket"); }).empty(),
      "unknown schemes are rejected");
  Expect(!StoreFailure([] { llmcc::compare::OpenStore("s3://"); }).empty(),
         "an S3 location needs a bucket");
  for (const char* location :
       {"s3://bucket/a/../b", "s3://bucket/./a", "s3://bucket/a//b"}) {
    Expect(StoreFailure([&] {
             llmcc::compare::OpenStore(location);
           }).find("invalid S3 store prefix") != std::string::npos,
           std::string("an S3 prefix with empty or dot segments is refused: ") +
               location);
  }
  Expect(
      StoreFailure([] {
        llmcc::compare::ParseStoreOptions({{"aws_secret_access_key", "leak"}});
      }).find("credentials come from the environment") != std::string::npos,
      "store options never carry credentials");
  const auto options =
      llmcc::compare::ParseStoreOptions({{"endpoint_url", "http://minio:9000"},
                                         {"region_name", "auto"},
                                         {"config", nullptr}});
  Expect(options.endpoint_url == "http://minio:9000" &&
             options.region_name == "auto",
         "endpoint and region options are read");

  // S3 requests: path-style with an endpoint, encoded keys, signed payload.
  {
    S3Fixture fixture;
    fixture.transport->Then(200, "payload");
    ExpectEq(fixture.store->Get("results/v2/a b+c.json"),
             std::optional<std::string>("payload"), "a GET returns the body");
    const HttpRequest& get = fixture.requests.at(0);
    ExpectEq(get.method, std::string("GET"), "reads use GET");
    ExpectEq(get.url,
             std::string("http://127.0.0.1:9000/bucket/llm-cc/results/v2/"
                         "a%20b%2Bc.json"),
             "path-style URLs encode each key byte");
    ExpectEq(get.sigv4, std::string("aws:amz:eu-west-1:s3"),
             "requests are signed for S3 in the configured region");
    Expect(HasHeader(get, "x-amz-content-sha256: " + llmcc::Sha256Hex("")),
           "a GET signs the empty payload");
    fixture.transport->Then(200);
    fixture.store->Put("entropy/v2/k.cbor", "cbor bytes");
    const HttpRequest& put = fixture.requests.at(1);
    Expect(put.method == "PUT" && put.body == "cbor bytes" &&
               HasHeader(put, "x-amz-content-sha256: " +
                                  llmcc::Sha256Hex("cbor bytes")),
           "a PUT signs its payload digest");
    Expect(!HasHeader(put, "x-amz-security-token: token"),
           "no session token without one");
  }
  {
    S3Fixture fixture({.bucket = "plain",
                       .region = "us-east-2",
                       .access_key_id = "AKID",
                       .secret_access_key = "secret",
                       .session_token = "token"});
    fixture.transport->Then(404);
    Expect(!fixture.store->Get("k").has_value(), "404 is a miss");
    fixture.transport->Then(404, "<Error><Code>NoSuchKey</Code></Error>");
    Expect(!fixture.store->Get("k").has_value(), "NoSuchKey is a miss");
    fixture.transport->Then(404, "<Error><Code>NoSuchBucket</Code></Error>");
    Expect(StoreFailure([&] {
             static_cast<void>(fixture.store->Get("k"));
           }).find("NoSuchBucket") != std::string::npos,
           "a missing bucket is an error, not a miss");
    ExpectEq(fixture.requests.at(0).url,
             std::string("https://plain.s3.us-east-2.amazonaws.com/k"),
             "AWS buckets use virtual-hosted URLs");
    Expect(HasHeader(fixture.requests.at(0), "x-amz-security-token: token"),
           "a session token is sent and signed");
    ExpectEq(llmcc::compare::S3Store(
                 {.bucket = "dotted.bucket", .region = "us-east-2"}, nullptr)
                 .ObjectUrl("k"),
             std::string("https://s3.us-east-2.amazonaws.com/dotted.bucket/k"),
             "dotted buckets use path-style URLs");
  }

  // 404 is a miss; authorization and exhausted retries are errors.
  {
    S3Fixture fixture;
    fixture.transport->Then(403, "<Error><Code>AccessDenied</Code></Error>");
    const std::string denied =
        StoreFailure([&] { static_cast<void>(fixture.store->Get("k")); });
    Expect(denied.find("HTTP 403") != std::string::npos &&
               denied.find("AccessDenied") != std::string::npos,
           "403 fails the read with the server's reason");
    Expect(fixture.sleeps.empty(), "authorization failures are not retried");
  }
  {
    S3Fixture fixture;
    fixture.transport->Then(503);
    fixture.transport->ThenFail();
    fixture.transport->Then(200, "late");
    ExpectEq(fixture.store->Get("k"), std::optional<std::string>("late"),
             "transient failures are retried");
    ExpectEq(fixture.sleeps.size(), std::size_t{2}, "two retries back off");
    Expect(fixture.sleeps[1] > fixture.sleeps[0], "backoff grows");
  }
  {
    S3Fixture fixture;
    fixture.transport->ThenFail();
    fixture.transport->ThenFail();
    fixture.transport->ThenFail();
    const std::string failure =
        StoreFailure([&] { static_cast<void>(fixture.store->Get("k")); });
    Expect(failure.find("after 3 attempts") != std::string::npos &&
               failure.find("connection reset") != std::string::npos,
           "persistent transport failures fail the read");
  }
  {
    S3Fixture fixture;
    fixture.transport->Then(429);
    fixture.transport->Then(500);
    fixture.transport->Then(500);
    Expect(!StoreFailure([&] { fixture.store->Put("k", "v"); }).empty(),
           "a write fails after its retries");
    ExpectEq(fixture.requests.size(), std::size_t{3}, "three attempts in all");
  }
  {
    S3Fixture fixture;
    Expect(
        !StoreFailure([&] { fixture.store->Put("../escape", "v"); }).empty() &&
            fixture.requests.empty(),
        "invalid keys never reach the network");
  }
  return 0;
}
