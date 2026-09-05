#include "src/backend_fetch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "src/sha256.h"
#include "src/test_util.h"

namespace fs = std::filesystem;

namespace {

using llmcc::test::Expect;
using llmcc::test::ExpectEq;

void Write(const fs::path& path, std::string_view contents) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!output) {
    throw std::runtime_error("could not write test file " + path.string());
  }
}

void WriteLittleEndian(std::string& output, std::size_t offset,
                       std::uint64_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    output[offset + index] = static_cast<char>(value >> (index * 8));
  }
}

unsigned char HexByte(std::string_view hex, std::size_t offset) {
  const auto digit = [](char value) -> unsigned char {
    return static_cast<unsigned char>(value <= '9' ? value - '0'
                                                   : value - 'a' + 10);
  };
  return static_cast<unsigned char>((digit(hex[offset]) << 4) |
                                    digit(hex[offset + 1]));
}

std::string Bundle(std::string_view name, bool valid_footer = true) {
  std::string body = "tiny backend body";
  std::string bundle = body + std::string(64, '\0');
  std::ranges::copy(name, bundle.begin() + std::ssize(body));
  WriteLittleEndian(bundle, body.size() + 16, 0);
  WriteLittleEndian(bundle, body.size() + 24, body.size());
  const std::string body_hash = llmcc::Sha256Hex(body);
  for (std::size_t index = 0; index < 32; ++index) {
    bundle[body.size() + 32 + index] =
        static_cast<char>(HexByte(body_hash, index * 2));
  }
  if (!valid_footer) {
    bundle[body.size() + 32] ^= 1;
  }
  return bundle;
}

struct FakeDownloads {
  struct Request {
    std::string url;
    std::string noun;
    bool show_progress;
    bool record_in_model_manifest;
  };

  std::map<std::string, std::string> files;
  std::vector<Request> requests;

  llmcc::BundleDownloader Downloader() {
    return [this](std::string_view url, const fs::path& target,
                  const llmcc::DownloadOptions& options) {
      requests.push_back(
          {.url = std::string(url),
           .noun = std::string(options.noun),
           .show_progress = options.show_progress,
           .record_in_model_manifest = options.record_in_model_manifest});
      const auto found = files.find(std::string(url));
      if (found == files.end()) {
        throw std::runtime_error("test URL not found");
      }
      Write(target, found->second);
      if (options.record_in_model_manifest) {
        Write(target.parent_path() / "models.json", "{}");
      }
    };
  }
};

template <typename Function>
std::string ExceptionMessage(Function function) {
  try {
    function();
  } catch (const std::exception& error) {
    return error.what();
  }
  return {};
}

llmcc::BackendFetchOptions Options(const fs::path& root,
                                   std::string_view name = "cuda") {
  return {.name = name,
          .version = "1.2.3",
          .git_sha = "",
          .build_identity = "test-build",
          .base_url = "",
          .explicit_url = std::nullopt,
          .runtime_root = root};
}

void AddValidBundle(FakeDownloads& downloads, std::string_view url,
                    std::string_view name = "cuda") {
  const std::string bundle = Bundle(name);
  downloads.files.emplace(std::string(url), bundle);
  downloads.files.emplace(std::string(url) + ".sha256",
                          llmcc::Sha256Hex(bundle) + "  bundle\n");
}

void TestBaseUrls(const fs::path& root) {
#if defined(__linux__) && defined(__x86_64__)
  ExpectEq(llmcc::BackendArtifactName("cuda"),
           std::optional<std::string>("llm-cc-backend-cuda-linux-x86_64"),
           "automatic artifact name matches the target platform");
#else
  Expect(!llmcc::BackendArtifactName("cuda").has_value(),
         "automatic fetching is disabled without a published platform");
#endif
  auto first_build = Options(root / "identity");
  first_build.build_identity = "first-build";
  auto second_build = first_build;
  second_build.build_identity = "second-build";
  Expect(llmcc::BackendBundlePath(first_build) !=
             llmcc::BackendBundlePath(second_build),
         "unstamped builds have distinct cache paths");
#if defined(__linux__) && defined(__x86_64__)
  for (const auto& [label, base] :
       std::vector<std::pair<std::string, std::string>>{
           {"release", "https://github.com/example/releases/download/v1.2.3"},
           {"resolver", "https://artifacts.example/pawelchcki/llm-cc/abc"},
       }) {
    const std::string url = base + "/llm-cc-backend-cuda-linux-x86_64.bundle";
    FakeDownloads downloads;
    AddValidBundle(downloads, url);
    auto options = Options(root / label);
    options.base_url = base;
    ExpectEq(llmcc::FetchBackendBundle(options, downloads.Downloader()),
             llmcc::BackendBundlePath(options), label + " bundle path");
    Expect(!downloads.requests.empty() && downloads.requests.front().url == url,
           label + " base produces the bundle URL");
  }
#endif

  auto options = Options(root / "empty-base");
  bool called = false;
  const std::string message = ExceptionMessage([&] {
    static_cast<void>(llmcc::FetchBackendBundle(
        options, [&](std::string_view, const fs::path&,
                     const llmcc::DownloadOptions&) { called = true; }));
  });
  Expect(message.find("no artifact base URL") != std::string::npos,
         "unstamped build explains the missing artifact base");
  Expect(message.find("--url") != std::string::npos,
         "unstamped build recommends --url");
  Expect(!called, "missing URL does not invoke the downloader");
}

void TestExplicitUrl(const fs::path& root) {
  const std::string explicit_url =
      "https://mirror.example/custom/backend.bundle";
  FakeDownloads downloads;
  AddValidBundle(downloads, explicit_url);
  auto options = Options(root);
  options.base_url = "https://ignored.example/release";
  options.explicit_url = explicit_url;
  static_cast<void>(llmcc::FetchBackendBundle(options, downloads.Downloader()));
  Expect(downloads.requests.size() >= 2, "bundle and checksum were downloaded");
  ExpectEq(downloads.requests[0].url, explicit_url,
           "explicit URL overrides base URL");
  ExpectEq(downloads.requests[1].url, explicit_url + ".sha256",
           "explicit checksum URL is derived from bundle URL");
  ExpectEq(downloads.requests[2].url,
           "https://mirror.example/custom/backend.manifest.json",
           "explicit manifest name is derived from the bundle URL");
}

void TestWholeFileMismatch(const fs::path& root) {
  const std::string base = "https://artifacts.example/release";
  const std::string url = base + "/llm-cc-backend-cuda-linux-x86_64.bundle";
  FakeDownloads downloads;
  downloads.files[url] = Bundle("cuda");
  downloads.files[url + ".sha256"] = std::string(64, '0') + "\n";
  auto options = Options(root);
  options.base_url = base;
  options.explicit_url = url;
  const std::string message = ExceptionMessage([&] {
    static_cast<void>(
        llmcc::FetchBackendBundle(options, downloads.Downloader()));
  });
  Expect(message.find("SHA-256 mismatch") != std::string::npos,
         "whole-file mismatch is rejected");
  Expect(!fs::exists(llmcc::BackendBundlePath(options)),
         "whole-file mismatch is removed from cache");
}

void TestFooterMismatch(const fs::path& root) {
  const std::string base = "https://artifacts.example/release";
  const std::string url = base + "/llm-cc-backend-cuda-linux-x86_64.bundle";
  const std::string bundle = Bundle("cuda", false);
  FakeDownloads downloads;
  downloads.files[url] = bundle;
  downloads.files[url + ".sha256"] = llmcc::Sha256Hex(bundle) + "\n";
  auto options = Options(root);
  options.base_url = base;
  options.explicit_url = url;
  const std::string message = ExceptionMessage([&] {
    static_cast<void>(
        llmcc::FetchBackendBundle(options, downloads.Downloader()));
  });
  Expect(message.find("footer SHA-256 mismatch") != std::string::npos,
         "footer body hash mismatch is rejected");
  Expect(!fs::exists(llmcc::BackendBundlePath(options)),
         "footer mismatch is removed from cache");
}

void TestManifestMismatch(const fs::path& root) {
  const std::string base = "https://artifacts.example/commit/expected";
  const std::string artifact = "llm-cc-backend-cuda-linux-x86_64";
  const std::string url = base + "/" + artifact + ".bundle";
  FakeDownloads downloads;
  AddValidBundle(downloads, url);
  downloads.files[base + "/" + artifact + ".manifest.json"] =
      R"({"name":"cuda","git_sha":"actual"})";
  auto options = Options(root);
  options.base_url = base;
  options.explicit_url = url;
  options.git_sha = "expected";
  const std::string message = ExceptionMessage([&] {
    static_cast<void>(
        llmcc::FetchBackendBundle(options, downloads.Downloader()));
  });
  Expect(message.find("backend bundle was built from a different commit") !=
             std::string::npos,
         "manifest commit mismatch is explained");
  Expect(message.find("expected") != std::string::npos &&
             message.find("actual") != std::string::npos,
         "manifest mismatch includes both commits");
  Expect(!fs::exists(llmcc::BackendBundlePath(options)),
         "commit mismatch is removed from cache");
}

void TestCachedBundle(const fs::path& root) {
  auto options = Options(root);
  options.base_url = "https://unused.example/release";
  const fs::path path = llmcc::BackendBundlePath(options);
  const std::string bundle = Bundle("cuda");
  Write(path, bundle);
  Write(path.string() + ".sha256", llmcc::Sha256Hex(bundle) + "\n");
  bool called = false;
  const fs::path result = llmcc::FetchBackendBundle(
      options,
      [&](std::string_view, const fs::path&, const llmcc::DownloadOptions&) {
        called = true;
        throw std::runtime_error("cached bundle should not download");
      });
  ExpectEq(result, path, "cached bundle path is returned");
  Expect(!called, "valid cached bundle does not invoke downloader");
}

void TestStampedCachedBundle(const fs::path& root) {
  auto options = Options(root);
  options.base_url = "https://unused.example/release";
  options.git_sha = "expected";
  const fs::path path = llmcc::BackendBundlePath(options);
  const fs::path manifest = path.parent_path() / "cuda.manifest.json";
  const std::string bundle = Bundle("cuda");
  Write(path, bundle);
  Write(path.string() + ".sha256", llmcc::Sha256Hex(bundle) + "\n");
  Write(manifest, R"({"git_sha":"actual"})");
  const std::string mismatch =
      ExceptionMessage([&] { llmcc::VerifyBackendBundle(options); });
  Expect(mismatch.find("built from a different commit") != std::string::npos,
         "cache hit verifies the manifest commit");

  Write(manifest, R"({"git_sha":"expected"})");
  bool called = false;
  ExpectEq(llmcc::FetchBackendBundle(
               options, [&](std::string_view, const fs::path&,
                            const llmcc::DownloadOptions&) { called = true; }),
           path, "matching stamped cache is accepted");
  Expect(!called, "matching stamped cache does not download");

  fs::remove(manifest);
  const std::string missing =
      ExceptionMessage([&] { llmcc::VerifyBackendBundle(options); });
  Expect(missing.find("manifest is required") != std::string::npos,
         "stamped cache hit requires its manifest");
}

void TestCachedFooter(const fs::path& root) {
  auto options = Options(root);
  options.base_url = "https://unused.example/release";
  const fs::path path = llmcc::BackendBundlePath(options);
  const std::string bundle = Bundle("cuda", false);
  Write(path, bundle);
  Write(path.string() + ".sha256", llmcc::Sha256Hex(bundle) + "\n");
  const std::string message =
      ExceptionMessage([&] { llmcc::VerifyBackendBundle(options); });
  Expect(message.find("footer SHA-256 mismatch") != std::string::npos,
         "cache hit verifies the bundle footer");
}

void TestInvalidCacheRepair(const fs::path& root) {
  const std::string base = "https://artifacts.example/commit/expected";
  const std::string artifact = "llm-cc-backend-cuda-linux-x86_64";
  const std::string url = base + "/" + artifact + ".bundle";
  FakeDownloads downloads;
  AddValidBundle(downloads, url);
  downloads.files[base + "/" + artifact + ".manifest.json"] =
      R"({"git_sha":"expected"})";

  auto options = Options(root);
  options.base_url = base;
  options.explicit_url = url;
  options.git_sha = "expected";
  const fs::path path = llmcc::BackendBundlePath(options);
  const std::string cached = Bundle("cuda");
  Write(path, cached);
  Write(path.string() + ".sha256", llmcc::Sha256Hex(cached) + "\n");
  Write(path.parent_path() / "cuda.manifest.json", R"({"git_sha":"wrong"})");

  ExpectEq(llmcc::FetchBackendBundle(options, downloads.Downloader()), path,
           "invalid stamped cache is repaired");
  Expect(!downloads.requests.empty(), "cache repair downloads a replacement");
  llmcc::VerifyBackendBundle(options);
}

void TestConcurrentFetch(const fs::path& root) {
  const std::string base = "https://artifacts.example/concurrent";
  const std::string artifact = "llm-cc-backend-cuda-linux-x86_64";
  const std::string bundle_url = base + "/" + artifact + ".bundle";
  const std::string bundle = Bundle("cuda");
  std::atomic<int> requests = 0;
  const llmcc::BundleDownloader downloader =
      [&](std::string_view url, const fs::path& target,
          const llmcc::DownloadOptions&) {
        ++requests;
        if (url == bundle_url) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          Write(target, bundle);
        } else if (url == bundle_url + ".sha256") {
          Write(target, llmcc::Sha256Hex(bundle) + "\n");
        } else if (url == base + "/" + artifact + ".manifest.json") {
          Write(target, R"({"git_sha":"expected"})");
        } else {
          throw std::runtime_error("unexpected concurrent test URL");
        }
      };
  auto options = Options(root);
  options.git_sha = "expected";
  options.base_url = base;
  options.explicit_url = bundle_url;
  std::exception_ptr first_error;
  std::exception_ptr second_error;
  std::thread first([&] {
    try {
      static_cast<void>(llmcc::FetchBackendBundle(options, downloader));
    } catch (...) {
      first_error = std::current_exception();
    }
  });
  std::thread second([&] {
    try {
      static_cast<void>(llmcc::FetchBackendBundle(options, downloader));
    } catch (...) {
      second_error = std::current_exception();
    }
  });
  first.join();
  second.join();
  if (first_error) {
    std::rethrow_exception(first_error);
  }
  if (second_error) {
    std::rethrow_exception(second_error);
  }
  ExpectEq(requests.load(), 3,
           "concurrent fetches perform one download transaction");
  llmcc::VerifyBackendBundle(options);
}

void TestDownloadSemantics(const fs::path& root) {
  const std::string base = "https://artifacts.example/release";
  const std::string artifact = "llm-cc-backend-cuda-linux-x86_64";
  const std::string url = base + "/" + artifact + ".bundle";
  FakeDownloads downloads;
  AddValidBundle(downloads, url);
  downloads.files[base + "/manifest.json"] = R"({"git_sha":"expected"})";
  auto options = Options(root);
  options.base_url = base;
  options.explicit_url = url;
  options.git_sha = "expected";

  const fs::path bundle =
      llmcc::FetchBackendBundle(options, downloads.Downloader());

  Expect(!fs::exists(bundle.parent_path() / "models.json"),
         "backend fetch does not create a model manifest");
  Expect(std::ranges::all_of(downloads.requests,
                             [](const auto& request) {
                               return request.noun == "backend bundle" &&
                                      !request.record_in_model_manifest;
                             }),
         "backend downloads are described and recorded as backend bundles");
  Expect(downloads.requests.size() == 4,
         "fallback manifest is requested after the artifact manifest");
  Expect(!downloads.requests[2].show_progress,
         "speculative artifact manifest request is quiet");
  Expect(downloads.requests[3].show_progress,
         "fallback manifest request reports progress");
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  const char* temporary = std::getenv("TEST_TMPDIR");
  Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const fs::path root = fs::path(temporary) / "backend-fetch";
  std::error_code error;
  fs::remove_all(root, error);

  TestBaseUrls(root / "bases");
  TestExplicitUrl(root / "explicit");
  TestWholeFileMismatch(root / "whole-file-mismatch");
  TestFooterMismatch(root / "footer-mismatch");
  TestManifestMismatch(root / "manifest-mismatch");
  TestCachedBundle(root / "cached");
  TestStampedCachedBundle(root / "stamped-cached");
  TestCachedFooter(root / "cached-footer");
  TestInvalidCacheRepair(root / "invalid-cache-repair");
  TestConcurrentFetch(root / "concurrent");
  TestDownloadSemantics(root / "download-semantics");
  return 0;
}
