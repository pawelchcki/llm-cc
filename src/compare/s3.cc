#include "src/compare/s3.h"

#include <curl/curl.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "src/download.h"
#include "src/sha256.h"

namespace llmcc::compare {
namespace {

// An object larger than this is refused rather than buffered.
constexpr std::uint64_t kMaxObjectBytes = kMaxStoreObjectBytes;
constexpr int kAttempts = 3;

void EnsureCurlInitialized() {
  static const bool initialized = [] {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      throw TransportError("failed to initialize libcurl");
    }
    return true;
  }();
  static_cast<void>(initialized);
}

template <typename Value>
void SetOption(CURL* curl, CURLoption option, Value value) {
  if (curl_easy_setopt(curl, option, value) != CURLE_OK) {
    throw TransportError("failed to configure libcurl");
  }
}

struct ResponseBuffer {
  std::string body;
  bool overflow = false;
};

std::size_t WriteBody(char* data, std::size_t size, std::size_t count,
                      void* context) {
  auto* buffer = static_cast<ResponseBuffer*>(context);
  const std::size_t bytes = size * count;
  if (buffer->body.size() + bytes > kMaxObjectBytes) {
    buffer->overflow = true;
    return 0;
  }
  buffer->body.append(data, bytes);
  return bytes;
}

class CurlTransport : public HttpTransport {
 public:
  HttpResponse Perform(const HttpRequest& request) override {
    EnsureCurlInitialized();
    const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(
        curl_easy_init(), curl_easy_cleanup);
    if (!curl) {
      throw TransportError("failed to create a libcurl handle");
    }
    std::array<char, CURL_ERROR_SIZE> error{};
    ResponseBuffer response;
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
        nullptr, curl_slist_free_all);
    const auto append = [&headers](const std::string& header) {
      curl_slist* next = curl_slist_append(headers.get(), header.c_str());
      if (next == nullptr) {
        throw TransportError("failed to build request headers");
      }
      // Appending returns the existing head, or a new one for an empty list.
      if (headers == nullptr) {
        headers.reset(next);
      }
    };
    for (const std::string& header : request.headers) {
      append(header);
    }
    // Never wait for a 100-continue round trip before sending a body.
    append("Expect:");
    CURL* handle = curl.get();
    SetOption(handle, CURLOPT_URL, request.url.c_str());
    SetOption(handle, CURLOPT_NOSIGNAL, 1L);
    SetOption(handle, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
    SetOption(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
    SetOption(handle, CURLOPT_LOW_SPEED_TIME, kStalledTransferTimeoutSeconds);
    SetOption(handle, CURLOPT_USERAGENT, "llm-cc/1");
    SetOption(handle, CURLOPT_ERRORBUFFER, error.data());
    SetOption(handle, CURLOPT_WRITEFUNCTION, &WriteBody);
    SetOption(handle, CURLOPT_WRITEDATA, &response);
    SetOption(handle, CURLOPT_PROTOCOLS_STR, "http,https");
    if (request.method == "PUT") {
      append("Content-Type: application/octet-stream");
      SetOption(handle, CURLOPT_CUSTOMREQUEST, "PUT");
      SetOption(handle, CURLOPT_POSTFIELDS, request.body.data());
      SetOption(handle, CURLOPT_POSTFIELDSIZE_LARGE,
                static_cast<curl_off_t>(request.body.size()));
    } else if (request.method != "GET") {
      throw TransportError("unsupported HTTP method " + request.method);
    }
    SetOption(handle, CURLOPT_HTTPHEADER, headers.get());
    if (!request.sigv4.empty()) {
      SetOption(handle, CURLOPT_AWS_SIGV4, request.sigv4.c_str());
      SetOption(handle, CURLOPT_USERNAME, request.access_key_id.c_str());
      SetOption(handle, CURLOPT_PASSWORD, request.secret_access_key.c_str());
    }
    std::string certificates;
    if (const auto bundle = CertificateBundle(); bundle.has_value()) {
      certificates = bundle->string();
      SetOption(handle, CURLOPT_CAINFO, certificates.c_str());
    }
    const CURLcode result = curl_easy_perform(handle);
    if (response.overflow) {
      throw TransportError("response exceeds " +
                           std::to_string(kMaxObjectBytes) + " bytes");
    }
    if (result != CURLE_OK) {
      const std::string detail =
          error[0] != '\0' ? error.data() : curl_easy_strerror(result);
      throw TransportError(detail);
    }
    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    return {.status = status, .body = std::move(response.body)};
  }
};

bool Retryable(long status) { return status == 429 || status >= 500; }

// The start of an error document, safe to print.
std::string Excerpt(std::string_view body) {
  std::string excerpt;
  for (const char character : body.substr(0, 300)) {
    excerpt.push_back(character >= ' ' && character <= '~' ? character : ' ');
  }
  return excerpt;
}

// The <Code> of an S3 error document, or empty.
std::string ErrorCode(std::string_view body) {
  constexpr std::string_view kOpen = "<Code>";
  const std::size_t start = body.find(kOpen);
  if (start == std::string_view::npos) {
    return {};
  }
  const std::size_t end = body.find("</Code>", start);
  return end == std::string_view::npos
             ? std::string()
             : std::string(body.substr(start + kOpen.size(),
                                       end - start - kOpen.size()));
}

}  // namespace

std::unique_ptr<HttpTransport> MakeCurlTransport() {
  return std::make_unique<CurlTransport>();
}

std::string EncodeS3Key(std::string_view key) {
  static constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string encoded;
  for (const char character : key) {
    const auto byte = static_cast<unsigned char>(character);
    if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
        byte == '.' || byte == '~' || byte == '/') {
      encoded.push_back(character);
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[byte >> 4U]);
      encoded.push_back(kHex[byte & 0xFU]);
    }
  }
  return encoded;
}

S3Store::S3Store(S3Settings settings, std::unique_ptr<HttpTransport> transport,
                 Sleep sleep)
    : settings_(std::move(settings)),
      transport_(std::move(transport)),
      sleep_(sleep ? std::move(sleep) : [](std::chrono::milliseconds delay) {
        std::this_thread::sleep_for(delay);
      }) {}

std::string S3Store::ObjectUrl(std::string_view key) const {
  const std::string object = settings_.prefix.empty()
                                 ? std::string(key)
                                 : settings_.prefix + "/" + std::string(key);
  if (settings_.endpoint.has_value()) {
    std::string base = *settings_.endpoint;
    while (!base.empty() && base.back() == '/') {
      base.pop_back();
    }
    return base + "/" + EncodeS3Key(settings_.bucket) + "/" +
           EncodeS3Key(object);
  }
  // Virtual-hosted addressing unless the bucket name cannot be a hostname
  // label under the wildcard certificate.
  if (settings_.bucket.find('.') == std::string::npos) {
    return "https://" + settings_.bucket + ".s3." + settings_.region +
           ".amazonaws.com/" + EncodeS3Key(object);
  }
  return "https://s3." + settings_.region + ".amazonaws.com/" +
         EncodeS3Key(settings_.bucket) + "/" + EncodeS3Key(object);
}

std::string S3Store::Describe() const {
  return "s3://" + settings_.bucket +
         (settings_.prefix.empty() ? "" : "/" + settings_.prefix);
}

HttpResponse S3Store::Send(std::string_view method, std::string_view key,
                           std::string_view body) {
  ValidateStoreKey(key);
  HttpRequest request{.method = std::string(method),
                      .url = ObjectUrl(key),
                      .headers = {"x-amz-content-sha256: " + Sha256Hex(body)},
                      .body = std::string(body),
                      .sigv4 = "aws:amz:" + settings_.region + ":s3",
                      .access_key_id = settings_.access_key_id,
                      .secret_access_key = settings_.secret_access_key};
  if (settings_.session_token.has_value()) {
    request.headers.push_back("x-amz-security-token: " +
                              *settings_.session_token);
  }
  std::string last_failure;
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    if (attempt != 0) {
      sleep_(std::chrono::milliseconds(500) * (1 << attempt));
    }
    try {
      HttpResponse response = transport_->Perform(request);
      if (!Retryable(response.status)) {
        return response;
      }
      last_failure = "HTTP " + std::to_string(response.status) + ": " +
                     Excerpt(response.body);
    } catch (const TransportError& error) {
      last_failure = error.what();
    }
  }
  throw StoreError(
      std::string(method == "GET" ? "cannot read" : "cannot write") + " " +
      Describe() + "/" + std::string(key) + " after " +
      std::to_string(kAttempts) + " attempts: " + last_failure);
}

std::optional<std::string> S3Store::Get(std::string_view key) {
  HttpResponse response = Send("GET", key, {});
  if (response.status == 200) {
    return std::move(response.body);
  }
  if (response.status == 404) {
    // A missing bucket is a 404 too; only a missing key is a cache miss.
    const std::string code = ErrorCode(response.body);
    if (code.empty() || code == "NoSuchKey") {
      return std::nullopt;
    }
  }
  throw StoreError("cannot read " + Describe() + "/" + std::string(key) +
                   ": HTTP " + std::to_string(response.status) + ": " +
                   Excerpt(response.body));
}

void S3Store::Put(std::string_view key, std::string_view value) {
  const HttpResponse response = Send("PUT", key, value);
  if (response.status != 200) {
    throw StoreError("cannot write " + Describe() + "/" + std::string(key) +
                     ": HTTP " + std::to_string(response.status) + ": " +
                     Excerpt(response.body));
  }
}

}  // namespace llmcc::compare
