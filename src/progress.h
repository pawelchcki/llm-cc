#ifndef LLM_CC_PROGRESS_H_
#define LLM_CC_PROGRESS_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <istream>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>

namespace llmcc {

std::string TerminalSafe(std::string_view text);

// A CLI-scoped observer. Library calls without a scope remain silent and
// noninteractive. The worker owns no inference objects and joins on unwind.
class ProgressReporter {
 public:
  using Clock = std::chrono::steady_clock;
  using Now = std::function<Clock::time_point()>;
  explicit ProgressReporter(
      std::string_view mode, std::ostream& output, Now now = Clock::now,
      std::chrono::milliseconds interval = std::chrono::seconds(5));
  explicit ProgressReporter(std::string_view mode);
  ~ProgressReporter();
  ProgressReporter(const ProgressReporter&) = delete;
  ProgressReporter& operator=(const ProgressReporter&) = delete;
  void Phase(std::string_view message);
  void StartFile(std::size_t index, std::size_t total,
                 const std::filesystem::path& path);
  void Tokens(std::size_t completed, std::size_t total);
  void Counter(std::uint64_t completed, std::uint64_t total,
               std::string_view unit);
  void FinishFile(bool cache_hit);
  void FailFile();
  void Heartbeat();  // Also permits deterministic clock-driven tests.
  void Message(std::string_view message);
  void Pause(bool paused);

 private:
  void Render(Clock::time_point now);
  bool enabled_;
  std::ostream& output_;
  Now now_;
  std::chrono::milliseconds interval_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool stopping_ = false;
  bool paused_ = false;
  std::string phase_;
  std::string file_;
  std::string unit_;
  std::uint64_t completed_ = 0;
  std::uint64_t total_ = 0;
  Clock::time_point started_;
  Clock::time_point advanced_;
  Clock::time_point rendered_;
  std::thread worker_;
};

class CliSession {
 public:
  CliSession(ProgressReporter& progress, bool assume_yes, bool no_download);
  ~CliSession();
  CliSession(const CliSession&) = delete;
  CliSession& operator=(const CliSession&) = delete;
  ProgressReporter& progress;
  bool assume_yes;
  bool no_download;

 private:
  CliSession* previous_;
};

bool CliSessionActive();
void ReportPhase(std::string_view phase);
void ReportCounter(std::uint64_t completed, std::uint64_t total,
                   std::string_view unit);
void ReportWarning(std::string_view message);
// The seam deliberately accepts a separate terminal stream, never scoring
// stdin.
void RequireDownloadConsent(bool assume_yes, bool no_download,
                            std::istream* terminal, std::ostream& output,
                            std::string_view description);
void ConfirmDownload(std::string_view url, const std::filesystem::path& target,
                     std::string_view description,
                     std::optional<std::uint64_t> bytes = std::nullopt);
void CheckDownloadAllowed();

}  // namespace llmcc
#endif  // LLM_CC_PROGRESS_H_
