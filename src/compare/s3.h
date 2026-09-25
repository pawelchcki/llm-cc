#ifndef LLM_CC_COMPARE_S3_H_
#define LLM_CC_COMPARE_S3_H_

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "src/compare/store.h"

namespace llmcc::compare {

struct HttpRequest {
  std::string method;
  std::string url;
  std::vector<std::string> headers;
  std::string body;
  // Signs the request with AWS Signature Version 4 when not empty, as
  // "aws:amz:REGION:SERVICE".
  std::string sigv4;
  std::string access_key_id;
  std::string secret_access_key;
};

struct HttpResponse {
  long status = 0;
  std::string body;
};

// The request never produced an HTTP response.
class TransportError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class HttpTransport {
 public:
  HttpTransport() = default;
  HttpTransport(const HttpTransport&) = delete;
  HttpTransport& operator=(const HttpTransport&) = delete;
  HttpTransport(HttpTransport&&) = delete;
  HttpTransport& operator=(HttpTransport&&) = delete;
  virtual ~HttpTransport() = default;
  virtual HttpResponse Perform(const HttpRequest& request) = 0;
};

// libcurl, which computes the SigV4 signature itself.
std::unique_ptr<HttpTransport> MakeCurlTransport();

struct S3Settings {
  std::string bucket;
  // Key prefix without leading or trailing '/'; may be empty.
  std::string prefix;
  // Path-style base URL such as http://127.0.0.1:9000; AWS when absent.
  std::optional<std::string> endpoint;
  std::string region = "us-east-1";
  std::string access_key_id;
  std::string secret_access_key;
  std::optional<std::string> session_token;
};

// Whole-object GET and PUT against S3 or a compatible server. Absent
// objects (404) are nullopt; 5xx, 429 and transport failures are retried
// twice, and every other status is a StoreError.
class S3Store : public Store {
 public:
  using Sleep = std::function<void(std::chrono::milliseconds)>;

  S3Store(S3Settings settings, std::unique_ptr<HttpTransport> transport,
          Sleep sleep = {});
  std::optional<std::string> Get(std::string_view key) override;
  void Put(std::string_view key, std::string_view value) override;
  [[nodiscard]] std::string Describe() const override;
  [[nodiscard]] std::string ObjectUrl(std::string_view key) const;

 private:
  HttpResponse Send(std::string_view method, std::string_view key,
                    std::string_view body);
  S3Settings settings_;
  std::unique_ptr<HttpTransport> transport_;
  Sleep sleep_;
};

// URI-encodes a key as SigV4 expects for S3: unreserved characters and '/'
// stay, everything else becomes %XX.
std::string EncodeS3Key(std::string_view key);

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_S3_H_
