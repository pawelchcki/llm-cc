#include "src/cache.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <vector>

#if !defined(_WIN32)
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "src/test_util.h"

#if !defined(_WIN32)
namespace {

void WaitForChild(pid_t child, std::string_view message) {
  int status = 0;
  llmcc::test::Expect(waitpid(child, &status, 0) == child &&
                          WIFEXITED(status) && WEXITSTATUS(status) == 0,
                      message);
}

void ConcurrentAcquire(const std::filesystem::path& target,
                       const std::filesystem::path& count) {
  const pid_t child = fork();
  llmcc::test::Expect(child >= 0, "fork acquisition child");
  if (child == 0) {
    try {
      llmcc::AcquireModel(
          "test", target,
          [&](std::string_view, const std::filesystem::path& output) {
            std::ofstream marker(count, std::ios::app);
            marker << "download\n";
            marker.close();
            usleep(150000);
            std::ofstream(output) << "model";
          });
      _exit(0);
    } catch (...) {
      _exit(1);
    }
  }
  WaitForChild(child, "concurrent model acquisition child succeeds");
}

}  // namespace
#endif

int main() {  // NOLINT(bugprone-exception-escape)
  namespace fs = std::filesystem;
  llmcc::test::ExpectEq(
      llmcc::CacheDirFrom(fs::path("/override"), fs::path("/xdg"),
                          fs::path("/home/user")),
      fs::path("/override"), "cache override precedence");
  llmcc::test::ExpectEq(llmcc::CacheDirFrom(std::nullopt, fs::path("/xdg"),
                                            fs::path("/home/user")),
                        fs::path("/xdg/llm-cc/models"), "XDG cache path");
  llmcc::test::ExpectEq(
      llmcc::CacheDirFrom(std::nullopt, std::nullopt, fs::path("/home/user")),
      fs::path("/home/user/.cache/llm-cc/models"), "home cache path");
  llmcc::test::ExpectEq(llmcc::FormatTimestamp(0),
                        std::string("1970-01-01 00:00:00 UTC"),
                        "epoch formatting");

  const char* temporary = std::getenv("TEST_TMPDIR");
  llmcc::test::Expect(temporary != nullptr, "TEST_TMPDIR is set");
  const fs::path cache = fs::path(temporary) / "cache";
  fs::create_directories(cache);
  std::ofstream(cache / "alpha.gguf") << "model";
  std::ofstream(cache / "ignored.txt") << "text";
  std::ostringstream models;
  llmcc::ListModels(cache, models);
  llmcc::test::Expect(models.str().find("alpha.gguf") != std::string::npos,
                      "GGUF listed");
  llmcc::test::Expect(models.str().find("ignored.txt") == std::string::npos,
                      "non-GGUF omitted");

  const fs::path symlink_loop = fs::path(temporary) / "symlink-loop";
  fs::create_symlink(symlink_loop.filename(), symlink_loop);
  bool list_failed = false;
  try {
    llmcc::ListModels(symlink_loop / "child", models);
  } catch (const std::runtime_error&) {
    list_failed = true;
  }
  llmcc::test::Expect(list_failed, "cache lookup errors are reported");

  std::ofstream(llmcc::PartialPath(cache / "alpha.gguf")) << "partial";
  llmcc::RemoveModel(cache, "alpha.gguf");
  llmcc::test::Expect(!fs::exists(cache / "alpha.gguf"), "model removed");
  llmcc::test::Expect(!fs::exists(cache / "alpha.gguf.partial"),
                      "partial removed");

  bool downloaded = false;
  const auto resolved =
      llmcc::ResolveModel(std::nullopt, false, fs::path(temporary), cache,
                          [&](const fs::path& target) {
                            downloaded = true;
                            std::ofstream(target) << "model";
                          });
  llmcc::test::Expect(downloaded, "missing default downloaded");
  llmcc::test::ExpectEq(resolved, cache / llmcc::kDefaultModelFile,
                        "cached default resolved");
#if !defined(_WIN32)
  const fs::path concurrent = fs::path(temporary) / "concurrent";
  fs::create_directories(concurrent);
  const fs::path target = concurrent / "shared.gguf";
  const fs::path count = concurrent / "downloads";
  const pid_t first = fork();
  llmcc::test::Expect(first >= 0, "fork first acquirer");
  if (first == 0) {
    try {
      llmcc::AcquireModel("test", target,
                          [&](std::string_view, const fs::path& output) {
                            std::ofstream marker(count, std::ios::app);
                            marker << "download\n";
                            marker.close();
                            usleep(150000);
                            std::ofstream(output) << "model";
                          });
      _exit(0);
    } catch (...) {
      _exit(1);
    }
  }
  ConcurrentAcquire(target, count);
  WaitForChild(first, "first concurrent acquirer succeeds");
  std::ifstream counts(count);
  std::string line;
  int downloads = 0;
  while (std::getline(counts, line)) ++downloads;
  llmcc::test::ExpectEq(downloads, 1, "one downloader for one target");

  int parallel_started[2]{};
  int parallel_release[2]{};
  llmcc::test::Expect(
      pipe(parallel_started) == 0 && pipe(parallel_release) == 0,
      "create parallel pipes");
  const auto spawn_parallel = [&](const fs::path& output) {
    const pid_t child = fork();
    llmcc::test::Expect(child >= 0, "fork parallel acquirer");
    if (child == 0) {
      try {
        llmcc::AcquireModel(
            "test", output, [&](std::string_view, const fs::path& destination) {
              const char signal = 's';
              static_cast<void>(write(parallel_started[1], &signal, 1));
              char release = '\0';
              if (read(parallel_release[0], &release, 1) != 1) _exit(1);
              std::ofstream(destination) << "model";
            });
        _exit(0);
      } catch (...) {
        _exit(1);
      }
    }
    return child;
  };
  const pid_t left = spawn_parallel(concurrent / "left.gguf");
  const pid_t right = spawn_parallel(concurrent / "right.gguf");
  struct pollfd readiness{
      .fd = parallel_started[0], .events = POLLIN, .revents = 0};
  llmcc::test::Expect(poll(&readiness, 1, 5000) == 1,
                      "parallel callbacks start");
  char signals[2]{};
  std::size_t received = 0;
  while (received != sizeof(signals)) {
    readiness.revents = 0;
    llmcc::test::Expect(poll(&readiness, 1, 5000) == 1,
                        "each parallel callback starts within timeout");
    const ssize_t count = read(parallel_started[0], signals + received,
                               sizeof(signals) - received);
    llmcc::test::Expect(count > 0, "read parallel callback signal");
    received += static_cast<std::size_t>(count);
  }
  llmcc::test::Expect(received == 2,
                      "different targets reach callbacks concurrently");
  const char releases[2] = {'r', 'r'};
  llmcc::test::Expect(write(parallel_release[1], releases, 2) == 2,
                      "release parallel callbacks");
  WaitForChild(left, "left parallel acquisition succeeds");
  WaitForChild(right, "right parallel acquisition succeeds");

  std::ofstream(concurrent / "one.gguf") << "one";
  std::ofstream(concurrent / "two.gguf") << "two";
  llmcc::MarkModelDownloaded(concurrent / "one.gguf");
  llmcc::MarkModelDownloaded(concurrent / "two.gguf");
  const auto manifest = llmcc::ReadManifest(concurrent);
  llmcc::test::Expect(
      manifest.contains("one.gguf") && manifest.contains("two.gguf"),
      "manifest retains unrelated records");

  // Removal takes the same target lock and therefore cannot delete a file
  // while its downloader still owns the resumable partial/target lock.
  const fs::path removing = concurrent / "remove-after-download.gguf";
  const fs::path removal_started = concurrent / "removal-started";
  const pid_t downloading = fork();
  llmcc::test::Expect(downloading >= 0, "fork removable acquisition");
  if (downloading == 0) {
    try {
      llmcc::AcquireModel("test", removing,
                          [&](std::string_view, const fs::path& output) {
                            std::ofstream(removal_started) << "started";
                            usleep(150000);
                            std::ofstream(output) << "model";
                          });
      _exit(0);
    } catch (...) {
      _exit(1);
    }
  }
  for (int attempts = 0; attempts != 100 && !fs::exists(removal_started);
       ++attempts)
    usleep(10000);
  llmcc::test::Expect(fs::exists(removal_started),
                      "download acquired target lock");
  llmcc::RemoveModel(concurrent, removing.filename().string());
  WaitForChild(downloading, "removable acquisition succeeds");
  llmcc::test::Expect(!fs::exists(removing),
                      "remove waits then removes target");

  // Independent manifest writers must leave parseable JSON containing records
  // they did not update.
  int manifest_started[2]{};
  int manifest_release[2]{};
  llmcc::test::Expect(
      pipe(manifest_started) == 0 && pipe(manifest_release) == 0,
      "create manifest pipes");
  const auto update_manifest = [&](const fs::path& path) {
    const pid_t child = fork();
    llmcc::test::Expect(child >= 0, "fork manifest writer");
    if (child == 0) {
      const char signal = 's';
      static_cast<void>(write(manifest_started[1], &signal, 1));
      char release = '\0';
      if (read(manifest_release[0], &release, 1) != 1) _exit(1);
      llmcc::MarkModelDownloaded(path);
      _exit(0);
    }
    return child;
  };
  std::vector<pid_t> manifest_children;
  for (int index = 0; index != 12; ++index) {
    const auto path =
        concurrent / ("concurrent-" + std::to_string(index) + ".gguf");
    std::ofstream(path) << index;
    manifest_children.push_back(update_manifest(path));
  }
  for (int index = 0; index != 12; ++index) {
    char signal;
    llmcc::test::Expect(read(manifest_started[0], &signal, 1) == 1,
                        "manifest writer ready");
  }
  for (int index = 0; index != 12; ++index) {
    const char release = 'r';
    llmcc::test::Expect(write(manifest_release[1], &release, 1) == 1,
                        "release manifest writer");
  }
  for (const pid_t child : manifest_children)
    WaitForChild(child, "manifest update succeeds");
  std::ifstream json_input(concurrent / "models.json");
  std::string json((std::istreambuf_iterator<char>(json_input)), {});
  const nlohmann::json parsed = nlohmann::json::parse(json);
  llmcc::test::Expect(
      parsed.contains("one.gguf") && parsed.contains("two.gguf"),
      "concurrent manifest retains records");
  for (int index = 0; index != 12; ++index)
    llmcc::test::Expect(
        parsed.contains("concurrent-" + std::to_string(index) + ".gguf"),
        "new manifest record retained");
#endif
  return 0;
}
