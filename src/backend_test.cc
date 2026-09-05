#include "src/backend.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "generated/build_config.h"
#include "src/backend_fetch.h"
#include "src/payload.h"
#include "src/sha256.h"
#include "src/test_util.h"

#if defined(LLMCC_TEST_LINUX_CPU) && !defined(LLM_CC_DYNAMIC_BACKENDS)
#error "Linux CPU builds must retain dynamic backend loading"
#endif

namespace {

template <typename Exception, typename Function>
bool ThrowsContaining(Function function, const std::string& needle) {
  try {
    function();
  } catch (const Exception& error) {
    return std::string(error.what()).find(needle) != std::string::npos;
  }
  return false;
}

void WriteFile(const std::filesystem::path& path,
               std::string_view contents = {}) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!output) {
    throw std::runtime_error("could not write test file: " + path.string());
  }
}

void WriteLittleEndian(std::span<char> output, std::size_t offset,
                       std::uint64_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    output[offset + index] = static_cast<char>(value >> (index * 8));
  }
}

void WriteBundleWithBadFooterHash(const std::filesystem::path& path) {
  constexpr std::array<char, 8> kMagic = {'L', 'L', 'M', 'C',
                                          'U', 'D', '0', '2'};
  constexpr std::size_t kBodySize = 12;
  constexpr std::size_t kFooterSize = 64;
  std::array<char, kBodySize + kFooterSize> contents{};
  std::ranges::copy(kMagic, contents.begin());
  contents[8] = 1;
  std::copy_n("cuda", 4, contents.begin() + kBodySize);
  WriteLittleEndian(contents, kBodySize + 16, 0);
  WriteLittleEndian(contents, kBodySize + 24, kBodySize);
  WriteFile(path, std::string_view(contents.data(), contents.size()));
}

std::string Bundle(std::string_view name) {
  std::string body = "tiny backend body";
  std::string bundle = body + std::string(64, '\0');
  std::ranges::copy(name, bundle.begin() + std::ssize(body));
  WriteLittleEndian(bundle, body.size() + 16, 0);
  WriteLittleEndian(bundle, body.size() + 24, body.size());
  const std::string body_hash = llmcc::Sha256Hex(body);
  const auto hex_digit = [](char value) -> unsigned char {
    return static_cast<unsigned char>(value <= '9' ? value - '0'
                                                   : value - 'a' + 10);
  };
  for (std::size_t index = 0; index < 32; ++index) {
    bundle[body.size() + 32 + index] =
        static_cast<char>((hex_digit(body_hash[index * 2]) << 4) |
                          hex_digit(body_hash[(index * 2) + 1]));
  }
  return bundle;
}

}  // namespace

int main() try {
  using llmcc::BackendDevice;
  using llmcc::BackendKind;
  using llmcc::SelectBackend;
  using llmcc::test::Expect;
  using llmcc::test::ExpectEq;

  ExpectEq(llmcc::ParseBackend("auto"), BackendKind::kAuto, "parse auto");
  ExpectEq(llmcc::ParseBackend("cpu"), BackendKind::kCpu, "parse cpu");
  ExpectEq(llmcc::ParseBackend("cuda"), BackendKind::kCuda, "parse cuda");
  ExpectEq(llmcc::ParseBackend("rocm"), BackendKind::kRocm, "parse rocm");
  Expect(ThrowsContaining<std::invalid_argument>(
             [] { static_cast<void>(llmcc::ParseBackend("metal")); },
             "auto, cpu, cuda, or rocm"),
         "invalid backend is explained");

  ExpectEq(SelectBackend(BackendKind::kAuto, 0, {}), BackendKind::kCpu,
           "auto without offload selects CPU");
  ExpectEq(SelectBackend(BackendKind::kCpu, 0, {}), BackendKind::kCpu,
           "explicit CPU without offload selects CPU");
  Expect(ThrowsContaining<std::invalid_argument>(
             [] { SelectBackend(BackendKind::kCpu, 1, {}); }, "--backend cpu"),
         "CPU offload is rejected");
  Expect(
      ThrowsContaining<std::runtime_error>(
          [] { SelectBackend(BackendKind::kCuda, 1, {}); }, "no usable CUDA"),
      "missing explicit CUDA is explained");
  Expect(
      ThrowsContaining<std::runtime_error>(
          [] { SelectBackend(BackendKind::kRocm, -1, {}); }, "no usable ROCm"),
      "missing explicit ROCm is explained");

  const std::vector<BackendDevice> devices = {
      {.backend = BackendKind::kCuda, .free_memory = 3},
      {.backend = BackendKind::kCuda, .free_memory = 5},
      {.backend = BackendKind::kRocm, .free_memory = 7},
  };
  ExpectEq(SelectBackend(BackendKind::kAuto, -1, devices), BackendKind::kCuda,
           "free VRAM is aggregated per family");

  const std::vector<BackendDevice> rocm_larger = {
      {.backend = BackendKind::kCuda, .free_memory = 8},
      {.backend = BackendKind::kRocm, .free_memory = 5},
      {.backend = BackendKind::kRocm, .free_memory = 5},
  };
  ExpectEq(SelectBackend(BackendKind::kAuto, 4, rocm_larger),
           BackendKind::kRocm, "larger aggregate ROCm memory wins");

  const std::vector<BackendDevice> tied = {
      {.backend = BackendKind::kCuda, .free_memory = 10},
      {.backend = BackendKind::kRocm, .free_memory = 10},
  };
  ExpectEq(SelectBackend(BackendKind::kAuto, 1, tied), BackendKind::kCuda,
           "CUDA wins ties");
  Expect(!llmcc::DeviceOutputGuaranteed(BackendKind::kCuda, 1, false),
         "partial offload does not guarantee device output");
  Expect(llmcc::DeviceOutputGuaranteed(BackendKind::kCuda, -1, false),
         "full CUDA offload guarantees device output");
  Expect(llmcc::DeviceOutputGuaranteed(BackendKind::kCpu, -1, true),
         "full Metal offload guarantees device output");
  Expect(!llmcc::DeviceOutputGuaranteed(BackendKind::kCpu, -1, false),
         "a CPU build cannot guarantee device output");
  Expect(llmcc::AutomaticBackendFetchEnabled() ==
             (LLM_CC_AUTO_FETCH_BACKENDS != 0),
         "runtime download policy follows generated build configuration");
  Expect(llmcc::AutomaticBackendFetchAllowed(true) ==
             (LLM_CC_AUTO_FETCH_BACKENDS != 0),
         "requested automatic download follows generated policy");
  Expect(!llmcc::AutomaticBackendFetchAllowed(false),
         "an explicit no-fetch request never permits downloading");

  namespace fs = std::filesystem;
  const char* temporary = std::getenv("TEST_TMPDIR");
  Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const fs::path root = fs::path(temporary) / "backend-resolution";
  std::error_code cleanup_error;
  fs::remove_all(root, cleanup_error);
  const fs::path backend_directory = root / "explicit";
  fs::path runtime_root = root / "runtime";
  fs::create_directories(backend_directory);
  fs::create_directories(runtime_root / "backends" / "test-version");
  const fs::path explicit_bundle = backend_directory / "cuda.bundle";
  const fs::path cached_bundle =
      runtime_root / "backends" / "test-version" / "cuda.bundle";
  WriteFile(explicit_bundle);
  WriteFile(backend_directory / "libllm-cc-backend-cuda.so");
  const std::string cached_contents = Bundle("cuda");
  WriteFile(cached_bundle, cached_contents);
  WriteFile(cached_bundle.string() + ".sha256",
            llmcc::Sha256Hex(cached_contents) + "\n");
  WriteFile(cached_bundle.parent_path() / "cuda.manifest.json",
            R"({"git_sha":"actual"})");
  bool embedded_probed = false;
  bool runtime_probed = false;
  const llmcc::ResolvedBackendPlugin resolved = llmcc::ResolveBackendPlugin(
      BackendKind::kCuda, backend_directory, std::span<const fs::path>{},
      [&] {
        embedded_probed = true;
        return false;
      },
      [&] {
        runtime_probed = true;
        return runtime_root;
      },
      "test-version");
  ExpectEq(resolved.path, explicit_bundle,
           "backend directory wins over runtime cache");
  ExpectEq(resolved.source, llmcc::BackendPluginSource::kBundle,
           "bundle wins over raw shared library");
  Expect(!embedded_probed && !runtime_probed,
         "later resolution stages are not probed");

  // A valid bundle installed under the executable prefix wins over development
  // runfiles and the user cache, while retaining normal bundle verification.
  const fs::path installed_root = root / "custom prefix" / "lib" / "llm-cc";
  const fs::path installed_bundle =
      llmcc::BackendBundlePath({.name = "cuda",
                                .version = "test-version",
                                .git_sha = "expected",
                                .runtime_root = installed_root});
  const std::string installed_contents = Bundle("cuda");
  fs::create_directories(installed_bundle.parent_path());
  WriteFile(installed_bundle, installed_contents);
  WriteFile(installed_bundle.string() + ".sha256",
            llmcc::Sha256Hex(installed_contents) + "\n");
  WriteFile(installed_bundle.parent_path() / "cuda.manifest.json",
            R"({"git_sha":"expected"})");
  const fs::path runfile_plugin = root / "runfiles" / "cuda.so";
  fs::create_directories(runfile_plugin.parent_path());
  WriteFile(runfile_plugin);
  const llmcc::ResolvedBackendPlugin installed = llmcc::ResolveBackendPlugin(
      BackendKind::kCuda, std::nullopt, std::array{runfile_plugin},
      [] { return false; }, [&] { return runtime_root; }, "test-version",
      "expected", {}, [&] { return std::optional<fs::path>(installed_root); });
  ExpectEq(installed.source, llmcc::BackendPluginSource::kInstalledBundle,
           "installed bundle wins over runfiles and cache");
  ExpectEq(installed.path, installed_bundle,
           "installed bundle uses the executable-prefix build key");

  const llmcc::ResolvedBackendPlugin embedded_before_installed =
      llmcc::ResolveBackendPlugin(
          BackendKind::kCuda, std::nullopt, std::span<const fs::path>{},
          [] { return true; }, [&] { return runtime_root; }, "test-version",
          "expected", {},
          [&] { return std::optional<fs::path>(installed_root); });
  ExpectEq(embedded_before_installed.source,
           llmcc::BackendPluginSource::kEmbedded,
           "embedded payload wins over installed bundle");

  WriteFile(installed_bundle.string() + ".sha256", std::string(64, '0'));
  Expect(ThrowsContaining<std::runtime_error>(
             [&] {
               static_cast<void>(llmcc::ResolveBackendPlugin(
                   BackendKind::kCuda, std::nullopt,
                   std::span<const fs::path>{}, [] { return false; },
                   [&] { return runtime_root; }, "test-version", "expected", {},
                   [&] { return std::optional<fs::path>(installed_root); }));
             },
             "reinstall with 'bazel run --config=cuda //:install'"),
         "bad installed checksum gives a reinstall hint");
  WriteFile(installed_bundle.string() + ".sha256",
            llmcc::Sha256Hex(installed_contents) + "\n");
  WriteFile(installed_bundle.parent_path() / "cuda.manifest.json",
            R"({"git_sha":"different"})");
  Expect(ThrowsContaining<std::runtime_error>(
             [&] {
               static_cast<void>(llmcc::ResolveBackendPlugin(
                   BackendKind::kCuda, std::nullopt,
                   std::span<const fs::path>{}, [] { return false; },
                   [&] { return runtime_root; }, "test-version", "expected", {},
                   [&] { return std::optional<fs::path>(installed_root); }));
             },
             "different commit"),
         "wrong installed commit is rejected");
  WriteFile(installed_bundle.parent_path() / "cuda.manifest.json",
            R"({"git_sha":"expected"})");
  WriteFile(installed_bundle, "bad footer");
  WriteFile(installed_bundle.string() + ".sha256",
            llmcc::Sha256Hex("bad footer") + "\n");
  Expect(ThrowsContaining<std::runtime_error>(
             [&] {
               static_cast<void>(llmcc::ResolveBackendPlugin(
                   BackendKind::kCuda, std::nullopt,
                   std::span<const fs::path>{}, [] { return false; },
                   [&] { return runtime_root; }, "test-version", "expected", {},
                   [&] { return std::optional<fs::path>(installed_root); }));
             },
             "footer is truncated"),
         "bad installed footer is rejected");
  WriteFile(installed_bundle, installed_contents);
  WriteFile(installed_bundle.string() + ".sha256",
            llmcc::Sha256Hex(installed_contents) + "\n");

  fs::remove(installed_bundle);
  fs::remove(installed_bundle.string() + ".sha256");
  WriteFile(installed_bundle.string() + ".partial", "partial");
  Expect(ThrowsContaining<std::runtime_error>(
             [&] {
               static_cast<void>(llmcc::ResolveBackendPlugin(
                   BackendKind::kCuda, std::nullopt,
                   std::span<const fs::path>{}, [] { return false; },
                   [&] { return runtime_root; }, "test-version", "expected", {},
                   [&] { return std::optional<fs::path>(installed_root); }));
             },
             "reinstall with 'bazel run --config=cuda //:install'"),
         "partial installed bundle gives a reinstall hint");
  fs::remove(installed_bundle.string() + ".partial");
  WriteFile(installed_bundle.string() + ".sha256.partial", "partial");
  Expect(ThrowsContaining<std::runtime_error>(
             [&] {
               static_cast<void>(llmcc::ResolveBackendPlugin(
                   BackendKind::kCuda, std::nullopt,
                   std::span<const fs::path>{}, [] { return false; },
                   [&] { return runtime_root; }, "test-version", "expected", {},
                   [&] { return std::optional<fs::path>(installed_root); }));
             },
             "reinstall with 'bazel run --config=cuda //:install'"),
         "partial installed checksum gives a reinstall hint");
  fs::remove(installed_bundle.string() + ".sha256.partial");

  Expect(ThrowsContaining<std::runtime_error>(
             [&] {
               static_cast<void>(llmcc::ResolveBackendPlugin(
                   BackendKind::kCuda, std::nullopt,
                   std::span<const fs::path>{}, [] { return false; },
                   [&] { return runtime_root; }, "test-version", "expected"));
             },
             "built from a different commit"),
         "ordinary cache resolution verifies the manifest commit");

  WriteFile(cached_bundle.parent_path() / "cuda.manifest.json",
            R"({"git_sha":"expected"})");
  const llmcc::ResolvedBackendPlugin cached = llmcc::ResolveBackendPlugin(
      BackendKind::kCuda, std::nullopt, std::span<const fs::path>{},
      [] { return false; }, [&] { return runtime_root; }, "test-version",
      "expected");
  Expect(cached.payload_verified,
         "validated runtime cache carries its payload verification");
  WriteFile(cached_bundle.parent_path() / "cuda.manifest.json",
            R"({"git_sha":"actual"})");

  bool repair_called = false;
  const fs::path repaired_bundle = root / "repaired.bundle";
  const llmcc::ResolvedBackendPlugin repaired = llmcc::ResolveBackendPlugin(
      BackendKind::kCuda, std::nullopt, std::span<const fs::path>{},
      [] { return false; }, [&] { return runtime_root; }, "test-version",
      "expected",
      [&] {
        repair_called = true;
        return std::optional<llmcc::ResolvedBackendPlugin>(
            {{.source = llmcc::BackendPluginSource::kBundle,
              .path = repaired_bundle,
              .payload_verified = true}});
      });
  Expect(repair_called, "invalid runtime cache reaches the fetch callback");
  ExpectEq(repaired.path, repaired_bundle,
           "cache repair returns the fetched bundle");
  Expect(repaired.payload_verified,
         "cache repair preserves payload verification");

  const fs::path missing_directory = root / "does-not-exist";
  Expect(ThrowsContaining<std::runtime_error>(
             [&] {
               static_cast<void>(llmcc::ResolveBackendPlugin(
                   BackendKind::kCuda, missing_directory,
                   std::span<const fs::path>{}, [] { return false; },
                   [&] { return runtime_root; }, "test-version"));
             },
             missing_directory.string()),
         "nonexistent explicit backend directory names the path");

  fs::path empty_runtime = root / "empty-runtime";
  fs::create_directories(empty_runtime);
  const auto resolve_missing = [&] {
    static_cast<void>(llmcc::ResolveBackendPlugin(
        BackendKind::kCuda, std::nullopt, std::span<const fs::path>{},
        [] { return false; }, [&] { return empty_runtime; }, "test-version"));
  };
  Expect(ThrowsContaining<std::runtime_error>(resolve_missing,
                                              "missing cuda backend plugin"),
         "absent plugin keeps the missing-plugin error");
  Expect(ThrowsContaining<std::runtime_error>(resolve_missing,
                                              "LLM_CC_BACKEND_DIR"),
         "missing-plugin error mentions the backend directory");
  const fs::path missing_cache =
      llmcc::BackendBundlePath({.name = "cuda",
                                .version = "test-version",
                                .runtime_root = empty_runtime});
  Expect(ThrowsContaining<std::runtime_error>(resolve_missing,
                                              missing_cache.string()),
         "missing-plugin error names the runtime cache path");

  bool automatic_fetch_attempted = false;
  const std::function<std::optional<llmcc::ResolvedBackendPlugin>()>
      source_policy_fetch = llmcc::AutomaticBackendFetchAllowed(true)
                                ? [&] {
                                    automatic_fetch_attempted = true;
                                    return std::optional<
                                        llmcc::ResolvedBackendPlugin>{};
                                  }
                                : std::function<std::optional<
                                      llmcc::ResolvedBackendPlugin>()>{};
  static_cast<void>(ThrowsContaining<std::runtime_error>(
      [&] {
        static_cast<void>(llmcc::ResolveBackendPlugin(
            BackendKind::kCuda, std::nullopt, std::span<const fs::path>{},
            [] { return false; }, [&] { return empty_runtime; }, "test-version",
            {}, source_policy_fetch));
      },
      "missing cuda backend plugin"));
  Expect(
      automatic_fetch_attempted == llmcc::AutomaticBackendFetchEnabled(),
      "automatic resolution attempts a download only in distribution builds");

#ifdef __linux__
  const fs::path corrupt_bundle = root / "corrupt.bundle";
  WriteBundleWithBadFooterHash(corrupt_bundle);
  Expect(ThrowsContaining<std::runtime_error>(
             [&] {
               static_cast<void>(llmcc::PrepareEmbeddedPayloadFromFile(
                   corrupt_bundle, "cuda"));
             },
             "footer SHA-256 mismatch"),
         "standalone bundle rejects a mismatched footer hash");
  Expect(ThrowsContaining<std::runtime_error>(
             [&] {
               static_cast<void>(llmcc::PrepareEmbeddedPayloadFromFile(
                   corrupt_bundle, "cuda"));
             },
             corrupt_bundle.string()),
         "standalone bundle error names the file");
#endif
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
  return EXIT_FAILURE;
}
