#include "src/progress.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace llmcc {
namespace {
thread_local CliSession* session = nullptr;

std::string PathUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::u8string value = path.u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
#else
  return path.string();
#endif
}
}  // namespace

std::string TerminalSafe(std::string_view text) {
  constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string safe;
  safe.reserve(text.size());
  const auto append_byte = [&](unsigned char byte) {
    safe.append("\\x");
    safe.push_back(kHex[byte >> 4]);
    safe.push_back(kHex[byte & 0x0f]);
  };
  const auto append_code_point = [&](std::uint32_t code_point) {
    safe.append("\\u");
    for (int shift = 12; shift >= 0; shift -= 4) {
      safe.push_back(kHex[(code_point >> shift) & 0x0f]);
    }
  };
  for (std::size_t index = 0; index < text.size();) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (byte < 0x20 || byte == 0x7f) {
      append_byte(byte);
      ++index;
      continue;
    }
    if (byte < 0x80) {
      safe.push_back(static_cast<char>(byte));
      ++index;
      continue;
    }
    std::size_t length = 0;
    if (byte >= 0xc2 && byte <= 0xdf) {
      length = 2;
    } else if (byte >= 0xe0 && byte <= 0xef) {
      length = 3;
    } else if (byte >= 0xf0 && byte <= 0xf4) {
      length = 4;
    }
    bool valid = length != 0 && index + length <= text.size();
    for (std::size_t offset = 1; valid && offset < length; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(text[index + offset]);
      valid = (continuation & 0xc0) == 0x80;
    }
    if (valid && length == 3) {
      const auto second = static_cast<unsigned char>(text[index + 1]);
      valid =
          (byte != 0xe0 || second >= 0xa0) && (byte != 0xed || second < 0xa0);
    } else if (valid && length == 4) {
      const auto second = static_cast<unsigned char>(text[index + 1]);
      valid =
          (byte != 0xf0 || second >= 0x90) && (byte != 0xf4 || second <= 0x8f);
    }
    if (!valid) {
      append_byte(byte);
      ++index;
      continue;
    }
    std::uint32_t leading_mask = 0x07;
    if (length == 2) {
      leading_mask = 0x1f;
    } else if (length == 3) {
      leading_mask = 0x0f;
    }
    std::uint32_t code_point = byte & leading_mask;
    for (std::size_t offset = 1; offset < length; ++offset) {
      code_point = (code_point << 6) |
                   (static_cast<unsigned char>(text[index + offset]) & 0x3f);
    }
    const bool bidi_control = code_point == 0x061c || code_point == 0x200e ||
                              code_point == 0x200f ||
                              (code_point >= 0x202a && code_point <= 0x202e) ||
                              (code_point >= 0x2066 && code_point <= 0x206f);
    if ((code_point >= 0x80 && code_point <= 0x9f) || bidi_control) {
      append_code_point(code_point);
      index += length;
      continue;
    }
    safe.append(text.substr(index, length));
    index += length;
  }
  return safe;
}

ProgressReporter::ProgressReporter(std::string_view mode)
    : ProgressReporter(mode, std::cerr) {}
ProgressReporter::ProgressReporter(std::string_view mode, std::ostream& output,
                                   Now now, std::chrono::milliseconds interval)
    : enabled_(mode != "never"),
      output_(output),
      now_(std::move(now)),
      interval_(interval),
      started_(now_()),
      advanced_(started_),
      rendered_(started_) {
  if (interval_ <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("progress interval must be positive");
  }
  if (mode != "auto" && mode != "always" && mode != "never") {
    throw std::invalid_argument("--progress expects auto, always, or never");
  }
  if (enabled_) {
    worker_ = std::thread([this] {
      std::unique_lock lock(mutex_);
      while (!stopping_) {
        const auto now = now_();
        const bool active = !paused_ && !phase_.empty();
        if (active && now - rendered_ >= interval_) Render(now);
        // Recompute the remaining delay after each phase/counter update so
        // updates between ticks cannot stretch a heartbeat to ten seconds.
        const auto delay = active ? interval_ - (now - rendered_) : interval_;
        changed_.wait_for(lock, delay, [this] { return stopping_; });
      }
    });
  }
}
ProgressReporter::~ProgressReporter() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  changed_.notify_all();
  if (worker_.joinable()) worker_.join();
}
void ProgressReporter::Render(Clock::time_point now) {
  rendered_ = now;
  output_ << "llm-cc: " << phase_;
  if (!file_.empty()) output_ << " file=" << file_;
  output_ << " elapsed_s="
          << std::chrono::duration_cast<std::chrono::seconds>(now - started_)
                 .count();
  if (!unit_.empty()) {
    output_ << ' ' << completed_ << '/'
            << (total_ == 0 ? "?" : std::to_string(total_)) << ' ' << unit_
            << " stalled_s="
            << std::chrono::duration_cast<std::chrono::seconds>(now - advanced_)
                   .count();
  }
  output_ << '\n' << std::flush;
}
void ProgressReporter::Phase(std::string_view message) {
  std::lock_guard lock(mutex_);
  phase_ = TerminalSafe(message);
  unit_.clear();
  completed_ = total_ = 0;
  advanced_ = now_();
  if (enabled_ && !paused_) Render(advanced_);
}
void ProgressReporter::StartFile(std::size_t index, std::size_t total,
                                 const std::filesystem::path& path) {
  {
    std::lock_guard lock(mutex_);
    file_ = "[" + std::to_string(index) + "/" + std::to_string(total) + "] " +
            TerminalSafe(PathUtf8(path));
  }
  Phase("analyzing");
}
void ProgressReporter::Counter(std::uint64_t completed, std::uint64_t total,
                               std::string_view unit) {
  std::lock_guard lock(mutex_);
  const auto now = now_();
  const bool advanced = completed != completed_ || unit_ != unit;
  if (advanced) advanced_ = now;
  const bool finished =
      (advanced || total != total_) && total != 0 && completed == total;
  completed_ = completed;
  total_ = total;
  unit_ = unit;
  if (enabled_ && !paused_ && (finished || now - rendered_ >= interval_))
    Render(now);
}
void ProgressReporter::Tokens(std::size_t completed, std::size_t total) {
  Counter(completed, total, "tokens");
}
void ProgressReporter::FinishFile(bool cache_hit) {
  Phase(cache_hit ? "complete cache=hit" : "complete cache=miss");
}
void ProgressReporter::FailFile() { Phase("failed"); }
void ProgressReporter::Heartbeat() {
  std::lock_guard lock(mutex_);
  const auto now = now_();
  if (enabled_ && !paused_ && !phase_.empty() && now - rendered_ >= interval_)
    Render(now);
}
void ProgressReporter::Message(std::string_view message) {
  std::lock_guard lock(mutex_);
  output_ << TerminalSafe(message) << '\n' << std::flush;
}
void ProgressReporter::Pause(bool paused) {
  std::lock_guard lock(mutex_);
  paused_ = paused;
}
CliSession::CliSession(ProgressReporter& reporter, bool yes, bool offline)
    : progress(reporter),
      assume_yes(yes),
      no_download(offline),
      previous_(session) {
  session = this;
}
CliSession::~CliSession() { session = previous_; }
bool CliSessionActive() { return session != nullptr; }
void ReportPhase(std::string_view phase) {
  if (session) session->progress.Phase(phase);
}
void ReportCounter(std::uint64_t completed, std::uint64_t total,
                   std::string_view unit) {
  if (session) session->progress.Counter(completed, total, unit);
}
void ReportWarning(std::string_view message) {
  if (session)
    session->progress.Message("warning: " + std::string(message));
  else
    std::cerr << "warning: " << message << '\n';
}
void CheckDownloadAllowed() {
  if (session && session->no_download) {
    throw std::runtime_error("--no-download forbids network access");
  }
}
void RequireDownloadConsent(bool assume_yes, bool no_download,
                            std::istream* terminal, std::ostream& output,
                            std::string_view description) {
  if (no_download)
    throw std::runtime_error("--no-download forbids network access");
  output << TerminalSafe(description) << '\n' << std::flush;
  if (assume_yes) return;
  if (terminal == nullptr) {
    throw std::runtime_error(
        "download requires confirmation through an interactive terminal; rerun "
        "with --assume-yes (or -y)");
  }
  output << "Download? [y/N] " << std::flush;
  std::string answer;
  const bool accepted =
      std::getline(*terminal, answer) &&
      (answer == "y" || answer == "Y" || answer == "yes" || answer == "YES");
  output << '\n' << std::flush;
  if (!accepted)
    throw std::runtime_error(
        "download declined; rerun with --assume-yes to accept");
}
void ConfirmDownload(std::string_view url, const std::filesystem::path& target,
                     std::string_view description,
                     std::optional<std::uint64_t> bytes) {
  if (!session) return;
  CheckDownloadAllowed();
  session->progress.Pause(true);
  try {
    const std::string prompt =
        std::string(description) + " source=" + std::string(url) +
        " destination=" + PathUtf8(target) + " size=" +
        (bytes ? std::to_string(*bytes) + " bytes (approximate)" : "unknown");
    if (session->assume_yes) {
      RequireDownloadConsent(true, session->no_download, nullptr, std::cerr,
                             prompt);
    } else {
#ifdef _WIN32
      std::ifstream terminal_input("CONIN$");
      std::ofstream terminal_output("CONOUT$");
      RequireDownloadConsent(
          false, session->no_download,
          terminal_input && terminal_output ? &terminal_input : nullptr,
          terminal_output ? static_cast<std::ostream&>(terminal_output)
                          : std::cerr,
          prompt);
#else
      std::fstream terminal("/dev/tty", std::ios::in | std::ios::out);
      RequireDownloadConsent(
          false, session->no_download, terminal ? &terminal : nullptr,
          terminal ? static_cast<std::ostream&>(terminal) : std::cerr, prompt);
#endif
    }
  } catch (...) {
    session->progress.Pause(false);
    throw;
  }
  session->progress.Pause(false);
}
}  // namespace llmcc
