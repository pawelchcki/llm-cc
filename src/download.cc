#include "src/download.h"

#include <curl/curl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include "src/cache.h"

namespace llmcc {
namespace {

class CurlGlobal {
 public:
  CurlGlobal() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      throw std::runtime_error("failed to initialize libcurl");
    }
  }
  CurlGlobal(const CurlGlobal&) = delete;
  CurlGlobal& operator=(const CurlGlobal&) = delete;
  ~CurlGlobal() { curl_global_cleanup(); }
};

using Curl = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;

struct WriteContext {
  std::ofstream* output;
  std::string error;
};

std::string FormatBytes(double bytes) {
  constexpr std::array<std::string_view, 5> kUnits = {"B", "KiB", "MiB", "GiB",
                                                      "TiB"};
  std::size_t unit = 0;
  while (bytes >= 1024.0 && unit + 1 < kUnits.size()) {
    bytes /= 1024.0;
    ++unit;
  }
  std::ostringstream output;
  output << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << bytes << ' '
         << kUnits[unit];
  return output.str();
}

class DownloadProgress {
 public:
  explicit DownloadProgress(std::uint64_t resume_offset)
      : resume_offset_(resume_offset),
        interactive_(isatty(STDERR_FILENO) != 0),
        started_(Clock::now()),
        last_update_(started_) {}

  DownloadProgress(const DownloadProgress&) = delete;
  DownloadProgress& operator=(const DownloadProgress&) = delete;

  static int Update(void* opaque, curl_off_t download_total,
                    curl_off_t downloaded, curl_off_t /*upload_total*/,
                    curl_off_t /*uploaded*/) {
    auto* progress = static_cast<DownloadProgress*>(opaque);
    progress->download_total_ = std::max<curl_off_t>(0, download_total);
    progress->downloaded_ = std::max<curl_off_t>(0, downloaded);
    const bool complete = progress->download_total_ > 0 &&
                          progress->downloaded_ >= progress->download_total_;
    progress->Render(complete);
    return 0;
  }

  void Finish() {
    Render(true);
    if (interactive_ && rendered_) {
      std::cerr << '\n';
    }
  }

 private:
  using Clock = std::chrono::steady_clock;

  void Render(bool force) {
    const auto now = Clock::now();
    if (force && rendered_ && download_total_ == rendered_total_ &&
        downloaded_ == rendered_downloaded_) {
      return;
    }
    if (!force && now - last_update_ < std::chrono::milliseconds(100)) {
      return;
    }
    if (!interactive_ && !force) {
      return;
    }
    last_update_ = now;

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::duration<double>>(now -
                                                                  started_)
            .count();
    const auto transferred = static_cast<std::uint64_t>(downloaded_);
    const std::uint64_t current = resume_offset_ + transferred;
    const double speed =
        elapsed > 0.0 ? static_cast<double>(transferred) / elapsed : 0.0;

    std::ostringstream line;
    if (download_total_ > 0) {
      const std::uint64_t total =
          resume_offset_ + static_cast<std::uint64_t>(download_total_);
      const double fraction = std::clamp(
          static_cast<double>(current) / static_cast<double>(total), 0.0, 1.0);
      constexpr std::size_t kBarWidth = 28;
      const auto filled =
          static_cast<std::size_t>(fraction * static_cast<double>(kBarWidth));
      line << '[' << std::string(filled, '#')
           << std::string(kBarWidth - filled, '-') << "] " << std::fixed
           << std::setprecision(1) << (fraction * 100.0) << "% "
           << FormatBytes(static_cast<double>(current)) << "/"
           << FormatBytes(static_cast<double>(total));
    } else {
      line << FormatBytes(static_cast<double>(current));
    }
    line << "  " << FormatBytes(speed) << "/s";

    const std::string rendered = line.str();
    if (interactive_) {
      std::cerr << '\r' << rendered;
      if (rendered.size() < previous_width_) {
        std::cerr << std::string(previous_width_ - rendered.size(), ' ');
      }
      std::cerr.flush();
      previous_width_ = rendered.size();
    } else {
      std::cerr << rendered << '\n';
    }
    rendered_ = true;
    rendered_total_ = download_total_;
    rendered_downloaded_ = downloaded_;
  }

  std::uint64_t resume_offset_;
  bool interactive_;
  Clock::time_point started_;
  Clock::time_point last_update_;
  curl_off_t download_total_ = 0;
  curl_off_t downloaded_ = 0;
  curl_off_t rendered_total_ = -1;
  curl_off_t rendered_downloaded_ = -1;
  std::size_t previous_width_ = 0;
  bool rendered_ = false;
};

std::size_t WriteBytes(char* contents, std::size_t size, std::size_t count,
                       void* opaque) {
  auto* context = static_cast<WriteContext*>(opaque);
  if (count != 0 && size > std::numeric_limits<std::size_t>::max() / count) {
    context->error = "download chunk size overflow";
    return 0;
  }
  const std::size_t bytes = size * count;
  context->output->write(contents, static_cast<std::streamsize>(bytes));
  if (!*context->output) {
    context->error = "failed to write partial download";
    return 0;
  }
  return bytes;
}

std::optional<std::filesystem::path> CertificateBundle() {
  const char* configured = std::getenv("SSL_CERT_FILE");
  if (configured != nullptr && *configured != '\0') {
    std::filesystem::path path(configured);
    if (std::filesystem::is_regular_file(path)) {
      return path;
    }
  }
  for (const char* candidate : {
           "/etc/ssl/certs/ca-certificates.crt",
           "/etc/pki/tls/certs/ca-bundle.crt",
           "/etc/ssl/ca-bundle.pem",
           "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
       }) {
    if (std::filesystem::is_regular_file(candidate)) {
      return std::filesystem::path(candidate);
    }
  }
  return std::nullopt;
}

void SetOption(CURL* curl, CURLoption option, long value) {
  if (curl_easy_setopt(curl, option, value) != CURLE_OK) {
    throw std::runtime_error("failed to configure libcurl");
  }
}

template <typename Value>
void SetOption(CURL* curl, CURLoption option, Value value) {
  if (curl_easy_setopt(curl, option, value) != CURLE_OK) {
    throw std::runtime_error("failed to configure libcurl");
  }
}

void FinishOutput(std::ofstream& output, const std::filesystem::path& partial) {
  output.flush();
  if (!output) {
    throw std::runtime_error("failed to write partial download " +
                             partial.string());
  }
  output.close();
  if (!output) {
    throw std::runtime_error("failed to close partial download " +
                             partial.string());
  }
}

}  // namespace

void StreamDownload(std::istream& input, const std::filesystem::path& target,
                    std::uint64_t resume_offset,
                    std::optional<std::uint64_t> total_length) {
  if (target.has_parent_path()) {
    std::filesystem::create_directories(target.parent_path());
  }
  const std::filesystem::path partial = PartialPath(target);
  if (resume_offset > 0) {
    std::error_code error;
    const std::uintmax_t actual = std::filesystem::file_size(partial, error);
    if (error || actual != resume_offset) {
      throw std::runtime_error("partial download does not match resume offset");
    }
  }
  std::ofstream output(
      partial,
      std::ios::binary | (resume_offset > 0 ? std::ios::app : std::ios::trunc));
  if (!output) {
    throw std::runtime_error("failed to open partial download " +
                             partial.string());
  }
  std::array<char, std::size_t{1024} * 1024> buffer{};
  std::uint64_t downloaded = resume_offset;
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      output.write(buffer.data(), count);
      downloaded += static_cast<std::uint64_t>(count);
    }
  }
  if (!input.eof()) {
    throw std::runtime_error("failed while downloading model");
  }
  if (!output) {
    throw std::runtime_error("failed to write partial download " +
                             partial.string());
  }
  if (total_length.has_value() && downloaded != *total_length) {
    throw std::runtime_error("download ended after " +
                             std::to_string(downloaded) + " bytes, expected " +
                             std::to_string(*total_length));
  }
  FinishOutput(output, partial);
  std::filesystem::rename(partial, target);
}

void DownloadFile(std::string_view download_url,
                  const std::filesystem::path& target,
                  const DownloadOptions& options) {
  const std::string url(download_url);
  if (target.has_parent_path()) {
    std::filesystem::create_directories(target.parent_path());
  }
  const std::filesystem::path partial = PartialPath(target);
  std::error_code file_error;
  std::uint64_t resume_offset = 0;
  if (std::filesystem::exists(partial, file_error)) {
    resume_offset =
        static_cast<std::uint64_t>(std::filesystem::file_size(partial));
  }
  CurlGlobal global;
  for (;;) {
    if (options.show_progress) {
      std::cerr << (resume_offset == 0 ? "Downloading " : "Resuming ")
                << options.noun
                << (resume_offset == 0 ? " from " : " download from ")
                << (resume_offset == 0
                        ? url
                        : std::to_string(resume_offset) + " bytes")
                << '\n';
    }

    Curl curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) {
      throw std::runtime_error("failed to create libcurl request");
    }
    std::ofstream output(
        partial, std::ios::binary |
                     (resume_offset > 0 ? std::ios::app : std::ios::trunc));
    if (!output) {
      throw std::runtime_error("failed to open partial download " +
                               partial.string());
    }
    WriteContext write_context{.output = &output, .error = {}};
    DownloadProgress progress(resume_offset);
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    SetOption(curl.get(), CURLOPT_URL, url.c_str());
    SetOption(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    SetOption(curl.get(), CURLOPT_FAILONERROR, 1L);
    SetOption(curl.get(), CURLOPT_USERAGENT, "llm-cc/1");
    SetOption(curl.get(), CURLOPT_ERRORBUFFER, error_buffer.data());
    SetOption(curl.get(), CURLOPT_WRITEFUNCTION, &WriteBytes);
    SetOption(curl.get(), CURLOPT_WRITEDATA, &write_context);
    SetOption(curl.get(), CURLOPT_NOPROGRESS, options.show_progress ? 0L : 1L);
    if (options.show_progress) {
      SetOption(curl.get(), CURLOPT_XFERINFOFUNCTION,
                &DownloadProgress::Update);
      SetOption(curl.get(), CURLOPT_XFERINFODATA, &progress);
    }
    if (resume_offset > 0) {
      SetOption(curl.get(), CURLOPT_RESUME_FROM_LARGE,
                static_cast<curl_off_t>(resume_offset));
    }
    const auto certificates = CertificateBundle();
    std::string certificate_path;
    if (certificates.has_value()) {
      certificate_path = certificates->string();
      SetOption(curl.get(), CURLOPT_CAINFO, certificate_path.c_str());
    }

    const CURLcode result = curl_easy_perform(curl.get());
    if (options.show_progress) {
      progress.Finish();
    }
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (resume_offset > 0 && status == 200) {
      output.close();
      std::filesystem::resize_file(partial, 0);
      resume_offset = 0;
      continue;
    }
    FinishOutput(output, partial);
    if (result != CURLE_OK || !write_context.error.empty()) {
      std::string detail = write_context.error;
      if (detail.empty()) {
        detail = error_buffer[0] != '\0' ? error_buffer.data()
                                         : curl_easy_strerror(result);
      }
      throw std::runtime_error("download failed for " + url + ": " + detail);
    }
    if ((resume_offset > 0 && status != 206) ||
        (resume_offset == 0 && status != 200)) {
      if (resume_offset > 0) {
        std::filesystem::resize_file(partial, resume_offset);
      }
      throw std::runtime_error("download returned unexpected HTTP status " +
                               std::to_string(status));
    }
    break;
  }
  std::filesystem::rename(partial, target);
  if (options.record_in_model_manifest) {
    MarkModelDownloaded(target);
  }
}

void DownloadModel(std::string_view model_url,
                   const std::filesystem::path& target) {
  DownloadFile(model_url, target,
               {.noun = "model",
                .show_progress = true,
                .record_in_model_manifest = true});
}

void DownloadDefaultModel(const std::filesystem::path& target) {
  DownloadModel(DefaultModel().url, target);
}

}  // namespace llmcc
