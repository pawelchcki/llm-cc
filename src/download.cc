#include "src/download.h"

#include <curl/curl.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
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
  output.close();
  std::filesystem::rename(partial, target);
}

void DownloadDefaultModel(const std::filesystem::path& target) {
  const std::string url(kDefaultModelUrl);
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
  std::cerr << (resume_offset == 0 ? "Downloading model from "
                                   : "Resuming model download from ")
            << (resume_offset == 0 ? std::string(kDefaultModelUrl)
                                   : std::to_string(resume_offset) + " bytes")
            << '\n';

  CurlGlobal global;
  Curl curl(curl_easy_init(), curl_easy_cleanup);
  if (!curl) {
    throw std::runtime_error("failed to create libcurl request");
  }
  std::ofstream output(
      partial,
      std::ios::binary | (resume_offset > 0 ? std::ios::app : std::ios::trunc));
  if (!output) {
    throw std::runtime_error("failed to open partial download " +
                             partial.string());
  }
  WriteContext write_context{.output = &output, .error = {}};
  std::array<char, CURL_ERROR_SIZE> error_buffer{};
  SetOption(curl.get(), CURLOPT_URL, url.c_str());
  SetOption(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
  SetOption(curl.get(), CURLOPT_FAILONERROR, 1L);
  SetOption(curl.get(), CURLOPT_USERAGENT, "llm-cc/1");
  SetOption(curl.get(), CURLOPT_ERRORBUFFER, error_buffer.data());
  SetOption(curl.get(), CURLOPT_WRITEFUNCTION, &WriteBytes);
  SetOption(curl.get(), CURLOPT_WRITEDATA, &write_context);
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
  output.flush();
  long status = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
  if (result != CURLE_OK || !write_context.error.empty()) {
    std::string detail = write_context.error;
    if (detail.empty()) {
      detail = error_buffer[0] != '\0' ? error_buffer.data()
                                       : curl_easy_strerror(result);
    }
    throw std::runtime_error("download failed for " +
                             std::string(kDefaultModelUrl) + ": " + detail);
  }
  if ((resume_offset > 0 && status != 206) ||
      (resume_offset == 0 && status != 200)) {
    output.close();
    if (resume_offset > 0) {
      std::filesystem::resize_file(partial, resume_offset);
    }
    throw std::runtime_error("download returned unexpected HTTP status " +
                             std::to_string(status));
  }
  output.close();
  std::filesystem::rename(partial, target);
  MarkModelDownloaded(target);
}

}  // namespace llmcc
