#include "src/download.h"

#include <curl/curl.h>

#include "src/progress.h"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "src/cache.h"

namespace llmcc {
namespace {

std::string PathUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::u8string value = path.u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
#else
  return path.string();
#endif
}
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

class DownloadProgress {
 public:
  DownloadProgress(std::uint64_t resume_offset, CURL* curl,
                   std::string_view noun, std::string_view url)
      : resume_offset_(resume_offset), curl_(curl), noun_(noun), url_(url) {}

  DownloadProgress(const DownloadProgress&) = delete;
  DownloadProgress& operator=(const DownloadProgress&) = delete;

  static int Update(void* opaque, curl_off_t download_total,
                    curl_off_t downloaded, curl_off_t /*upload_total*/,
                    curl_off_t /*uploaded*/) {
    auto* progress = static_cast<DownloadProgress*>(opaque);
    progress->download_total_ = std::max<curl_off_t>(0, download_total);
    progress->downloaded_ = std::max<curl_off_t>(0, downloaded);
    progress->Report();
    return 0;
  }

  static int Resolving(void* /*resolver*/, void* /*reserved*/, void* opaque) {
    static_cast<DownloadProgress*>(opaque)->Phase("resolving");
    return 0;
  }

  static int Connecting(void* opaque, curl_socket_t /*socket*/,
                        curlsocktype /*purpose*/) {
    static_cast<DownloadProgress*>(opaque)->Phase("connecting");
    return CURL_SOCKOPT_OK;
  }

  static int Fetching(void* opaque, char* /*remote_ip*/, char* /*local_ip*/,
                      int /*remote_port*/, int /*local_port*/) {
    static_cast<DownloadProgress*>(opaque)->Phase("fetching");
    return CURL_PREREQFUNC_OK;
  }

  void Phase(std::string_view phase) {
    char* effective_url = nullptr;
    curl_easy_getinfo(curl_, CURLINFO_EFFECTIVE_URL, &effective_url);
    const std::string description =
        std::string(phase) + " " + noun_ + " from " +
        SanitizeUrlForDiagnostic(effective_url ? effective_url : url_);
    if (phase_ == description) return;
    phase_ = description;
    ReportPhase(phase_);
    Report();
  }

  void Finish() {
    // Close-delimited responses learn their final size only at EOF.
    if (download_total_ == 0) download_total_ = downloaded_;
    Report();
  }

 private:
  void Report() const {
    const std::uint64_t total =
        download_total_ > 0 ? resume_offset_ + download_total_ : 0;
    ReportCounter(resume_offset_ + downloaded_, total, "bytes", resume_offset_);
  }

  std::uint64_t resume_offset_;
  CURL* curl_;
  std::string noun_;
  std::string url_;
  std::string phase_;
  curl_off_t download_total_ = 0;
  curl_off_t downloaded_ = 0;
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

std::optional<std::filesystem::path> CertificateBundle() {
#if defined(_WIN32)
  const wchar_t* configured = _wgetenv(L"SSL_CERT_FILE");
  if (configured != nullptr && *configured != L'\0') {
#else
  const char* configured = std::getenv("SSL_CERT_FILE");
  if (configured != nullptr && *configured != '\0') {
#endif
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
  // A 1 MiB automatic buffer exhausts the default Windows thread stack.
  std::vector<char> buffer(std::size_t{1024} * 1024);
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
#if defined(_WIN32)
  if (!MoveFileExW(partial.c_str(), target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(),
                            "failed to install downloaded file");
  }
#else
  std::filesystem::rename(partial, target);
#endif
}

std::string DownloadFailureMessage(std::string_view url, long status,
                                   std::string_view detail, bool timed_out) {
  std::string message = "download failed for " + std::string(url) + " (HTTP " +
                        std::to_string(status) + "): " + std::string(detail);
  if (timed_out) {
    message +=
        " (15-second connection / 60-second stalled-transfer timeout; partial "
        "download preserved)";
  }
  return message;
}

void DownloadFile(std::string_view download_url,
                  const std::filesystem::path& target,
                  const DownloadOptions& options) {
  CheckDownloadAllowed();
  // Share CLI progress and its independent heartbeat with explicit standalone
  // progress requests, including when stderr is redirected to a pipe or log.
  std::unique_ptr<ProgressReporter> owned_progress;
  std::unique_ptr<CliSession> owned_session;
  if (options.show_progress && !CliSessionActive()) {
    owned_progress = std::make_unique<ProgressReporter>("auto");
    owned_session = std::make_unique<CliSession>(*owned_progress, true, false);
  }
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
    DownloadProgress progress(resume_offset, curl.get(), options.noun, url);
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    SetOption(curl.get(), CURLOPT_URL, url.c_str());
    SetOption(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    SetOption(curl.get(), CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
    SetOption(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
    SetOption(curl.get(), CURLOPT_LOW_SPEED_TIME,
              kStalledTransferTimeoutSeconds);
    SetOption(curl.get(), CURLOPT_FAILONERROR, 1L);
    SetOption(curl.get(), CURLOPT_USERAGENT, "llm-cc/1");
    SetOption(curl.get(), CURLOPT_ERRORBUFFER, error_buffer.data());
    SetOption(curl.get(), CURLOPT_WRITEFUNCTION, &WriteBytes);
    SetOption(curl.get(), CURLOPT_WRITEDATA, &write_context);
    const bool observe = options.show_progress || CliSessionActive();
    SetOption(curl.get(), CURLOPT_NOPROGRESS, observe ? 0L : 1L);
    if (observe) {
      SetOption(curl.get(), CURLOPT_XFERINFOFUNCTION,
                &DownloadProgress::Update);
      SetOption(curl.get(), CURLOPT_XFERINFODATA, &progress);
      SetOption(curl.get(), CURLOPT_RESOLVER_START_FUNCTION,
                &DownloadProgress::Resolving);
      SetOption(curl.get(), CURLOPT_RESOLVER_START_DATA, &progress);
      SetOption(curl.get(), CURLOPT_SOCKOPTFUNCTION,
                &DownloadProgress::Connecting);
      SetOption(curl.get(), CURLOPT_SOCKOPTDATA, &progress);
      SetOption(curl.get(), CURLOPT_PREREQFUNCTION,
                &DownloadProgress::Fetching);
      SetOption(curl.get(), CURLOPT_PREREQDATA, &progress);
    }
    if (resume_offset > 0) {
      SetOption(curl.get(), CURLOPT_RESUME_FROM_LARGE,
                static_cast<curl_off_t>(resume_offset));
    }
    const auto certificates = CertificateBundle();
    std::string certificate_path;
    if (certificates.has_value()) {
      certificate_path = PathUtf8(*certificates);
      SetOption(curl.get(), CURLOPT_CAINFO, certificate_path.c_str());
    }

    progress.Phase("resolving");
    const CURLcode result = curl_easy_perform(curl.get());
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    char* effective_url = nullptr;
    curl_easy_getinfo(curl.get(), CURLINFO_EFFECTIVE_URL, &effective_url);
    const std::string failed_url =
        SanitizeUrlForDiagnostic(effective_url ? effective_url : url);
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
      throw std::runtime_error(DownloadFailureMessage(
          failed_url, status, detail, result == CURLE_OPERATION_TIMEDOUT));
    }
    if ((resume_offset > 0 && status != 206) ||
        (resume_offset == 0 && status != 200)) {
      if (resume_offset > 0) {
        std::filesystem::resize_file(partial, resume_offset);
      }
      throw std::runtime_error("download returned unexpected HTTP status " +
                               std::to_string(status) + " for " + failed_url);
    }
    progress.Finish();
    break;
  }
#if defined(_WIN32)
  if (!MoveFileExW(partial.c_str(), target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(),
                            "failed to install downloaded file");
  }
#else
  std::filesystem::rename(partial, target);
#endif
  if (options.record_in_model_manifest) {
    MarkModelDownloaded(target);
  }
}

void DownloadModel(std::string_view model_url,
                   const std::filesystem::path& target) {
  std::optional<std::uint64_t> bytes;
  for (const auto& model : Models())
    if (model.url == model_url) bytes = model.approx_bytes;
  ConfirmDownload(model_url, target, "model", bytes);
  DownloadFile(model_url, target,
               {.noun = "model",
                .show_progress = true,
                .record_in_model_manifest = true});
}

void DownloadDefaultModel(const std::filesystem::path& target) {
  DownloadModel(DefaultModel().url, target);
}

}  // namespace llmcc
