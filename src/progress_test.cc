#include "src/progress.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "src/backend.h"
#include "src/cache.h"
#include "src/test_util.h"

namespace {
using llmcc::test::Expect;
template <typename F>
bool Fails(F work, std::string_view message) {
  try {
    work();
  } catch (const std::exception& error) {
    return std::string(error.what()).find(message) != std::string::npos;
  }
  return false;
}
}  // namespace

int main() {
  using namespace std::chrono_literals;
  using llmcc::BackendKind;
  for (auto backend :
       {BackendKind::kAuto, BackendKind::kCuda, BackendKind::kRocm}) {
    Expect(llmcc::ResolveExecutionOptions(backend, std::nullopt, false)
                   .gpu_layers == -1,
           "omitted offload means all layers");
    Expect(llmcc::ResolveExecutionOptions(backend, 7, false).gpu_layers == 7,
           "partial offload retained");
  }
  for (auto backend : {BackendKind::kAuto, BackendKind::kCpu}) {
    Expect(llmcc::ResolveExecutionOptions(backend, 0, false).backend ==
               BackendKind::kCpu,
           "explicit zero opts into CPU");
    Expect(llmcc::ResolveExecutionOptions(backend, std::nullopt, true)
                   .gpu_layers == 0,
           "force CPU sets zero");
  }
  Expect(llmcc::ResolveExecutionOptions(BackendKind::kCpu, std::nullopt, false)
                 .gpu_layers == 0,
         "backend CPU shorthand");
  for (auto backend : {BackendKind::kCuda, BackendKind::kRocm}) {
    Expect(Fails([&] { llmcc::ResolveExecutionOptions(backend, 0, false); },
                 "contradictory"),
           "accelerator plus zero rejected");
    Expect(Fails(
               [&] {
                 llmcc::ResolveExecutionOptions(backend, std::nullopt, true);
               },
               "contradictory"),
           "force CPU plus accelerator rejected");
  }
  Expect(
      Fails(
          [] { llmcc::ResolveExecutionOptions(BackendKind::kCpu, -1, false); },
          "contradictory"),
      "CPU plus offload rejected");
  Expect(
      Fails([] { llmcc::ResolveExecutionOptions(BackendKind::kAuto, 1, true); },
            "contradictory"),
      "force CPU plus partial offload rejected");
  Expect(llmcc::SmallerModelGuidance(2'000'000'000).find("1.5B may fit") !=
             std::string::npos,
         "memory-based suggestion");

  const auto recovery = [](std::vector<std::string> arguments,
                           bool score = false) {
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    return llmcc::CpuRecoveryCommand(static_cast<int>(argv.size()), argv.data(),
                                     score);
  };
  llmcc::test::ExpectEq(
      recovery({"score", "--backend", "cuda", "--model", "model.gguf",
                "--prompt", "--backend", "--gpu-layers", "-1",
                "--entropy-reduction", "device", "--entropy", "--no-download"},
               true),
#ifdef _WIN32
      std::string("CPU rerun: llm-cc score \"--model\" \"model.gguf\" "
                  "\"--prompt\" \"--backend\" \"--entropy\" "
                  "\"--no-download\" --force-cpu"),
#else
      std::string("CPU rerun: llm-cc score '--model' 'model.gguf' '--prompt' "
                  "'--backend' '--entropy' '--no-download' --force-cpu"),
#endif
      "CPU recovery preserves flag-like prompt text and scoring switches");
  llmcc::test::ExpectEq(
      recovery({"llm-cc", "--force-cpu", "--no-cache", "--include-headers",
                "--model", "--force-cpu", "source's file.cc", "-y",
                "--backend-dir", "backends"}),
#ifdef _WIN32
      std::string("CPU rerun: llm-cc \"--no-cache\" \"--include-headers\" "
                  "\"--model\" \"--force-cpu\" \"source's file.cc\" \"-y\" "
                  "--force-cpu"),
#else
      std::string(
          "CPU rerun: llm-cc '--no-cache' '--include-headers' '--model' "
          "'--force-cpu' 'source'\\''s file.cc' '-y' --force-cpu"),
#endif
      "CPU recovery preserves model paths, source quoting and analysis flags");

  std::ostringstream consent;
  for (auto answer : {"y\n", "Y\n", "yes\n"}) {
    std::istringstream terminal(answer);
    llmcc::RequireDownloadConsent(false, false, &terminal, consent,
                                  "source destination size");
  }
  for (auto answer : {"", "\n", "n\n"}) {
    std::istringstream terminal(answer);
    Expect(Fails(
               [&] {
                 llmcc::RequireDownloadConsent(false, false, &terminal, consent,
                                               "asset");
               },
               "declined"),
           "EOF and default refusal");
  }
  Expect(Fails(
             [&] {
               llmcc::RequireDownloadConsent(false, false, nullptr, consent,
                                             "asset");
             },
             "--assume-yes"),
         "no terminal fails promptly");
  llmcc::RequireDownloadConsent(true, false, nullptr, consent, "asset");
  Expect(Fails(
             [&] {
               llmcc::RequireDownloadConsent(true, true, nullptr, consent,
                                             "asset");
             },
             "--no-download"),
         "offline overrides yes");
  Expect(consent.str().find("[y/N]") != std::string::npos, "default no prompt");

  std::atomic<int> ticks = 0;
  std::ostringstream output;
  {
    llmcc::ProgressReporter progress("auto", output, [&] {
      return llmcc::ProgressReporter::Clock::time_point{} +
             std::chrono::seconds(ticks.load());
    });
    progress.StartFile(1, 2, "example.rs");
    progress.Phase("inference");
    progress.Tokens(0, 50);
    const auto initial = output.str();
    ticks = 4;
    progress.Heartbeat();
    Expect(output.str() == initial, "heartbeat throttled");
    ticks = 5;
    progress.Heartbeat();
    Expect(output.str().find("0/50 tokens stalled_s=5") != std::string::npos,
           "heartbeat without completed batches");
    progress.Tokens(10, 50);
    ticks = 10;
    progress.Heartbeat();
    Expect(output.str().find("10/50 tokens stalled_s=5") != std::string::npos,
           "counter advance resets stalled age");
    Expect(output.str().find("example.rs") != std::string::npos,
           "phase retains current file");
    progress.Phase("downloading");
    progress.Counter(25, 0, "bytes");
    ticks = 15;
    progress.Heartbeat();
    Expect(output.str().find("25/? bytes stalled_s=5") != std::string::npos,
           "unknown transfer size is distinct from a known total");
    // Learning the final size must report completion even if no more bytes
    // arrive, without waiting for the next heartbeat.
    progress.Counter(25, 25, "bytes");
    Expect(output.str().find("25/25 bytes") != std::string::npos,
           "completion is reported when the total becomes known");
    const auto complete = output.str();
    progress.Counter(25, 25, "bytes");
    Expect(output.str() == complete, "repeated completion is throttled");
  }
  const auto stopped = output.str();
  std::this_thread::sleep_for(20ms);
  Expect(output.str() == stopped, "worker stopped");
  // Worker advances while the calling thread blocks and joins on exception.
  std::ostringstream blocked;
  try {
    llmcc::ProgressReporter progress("always", blocked,
                                     llmcc::ProgressReporter::Clock::now, 10ms);
    progress.Phase("blocked model load");
    std::this_thread::sleep_for(65ms);
    throw std::runtime_error("fixture");
  } catch (const std::runtime_error&) {
  }
  const auto first = blocked.str().find("blocked model load");
  Expect(first != std::string::npos &&
             blocked.str().find("blocked model load", first + 1) !=
                 std::string::npos,
         "scoped worker reports blocked work independently");
  std::ostringstream silent;
  {
    llmcc::ProgressReporter progress("never", silent);
    progress.Phase("hidden");
    progress.Tokens(1, 2);
    progress.Heartbeat();
    Expect(silent.str().empty(), "never suppresses routine status");
    progress.Message("warning: retained");
  }
  Expect(silent.str() == "warning: retained\n", "warnings retained");

  const auto target =
      std::filesystem::path(std::getenv("TEST_TMPDIR")) / "acquire.gguf";
  int requests = 0;
  auto download = [&](std::string_view url, const std::filesystem::path& path) {
    llmcc::ConfirmDownload(url, path, "model", 4);
    ++requests;
    std::ofstream(path) << "GGUF";
  };
  llmcc::ProgressReporter progress("never");
  {
    llmcc::CliSession session(progress, true, false);
    llmcc::AcquireModel("https://example.invalid/model", target, download);
  }
  {
    llmcc::CliSession session(progress, false, true);
    llmcc::AcquireModel("https://example.invalid/model", target, download);
  }
  Expect(requests == 1,
         "warm acquisition never asks or downloads even offline");
  return 0;
}
