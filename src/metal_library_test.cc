#include <ggml-backend.h>
#include <ggml-metal.h>
#include <ggml.h>

#include <array>
#include <mutex>
#include <string>
#include <thread>

#include "src/test_util.h"

namespace {

struct Log {
  std::mutex mutex;
  std::string text;

  std::string Read() {
    std::lock_guard lock(mutex);
    return text;
  }
};

void Capture(ggml_log_level, const char* text, void* context) {
  auto& log = *static_cast<Log*>(context);
  std::lock_guard lock(log.mutex);
  log.text += text;
}

}  // namespace

int main() {
  using llmcc::test::Expect;
  Log log;
  ggml_log_set(Capture, &log);
  auto registry = ggml_backend_metal_reg();
  Expect(ggml_backend_reg_dev_count(registry) > 0,
         "Apple Silicon runner exposes a Metal device");
  Expect(log.Read().find("using embedded metal library") == std::string::npos,
         "device discovery does not compile shaders");

  std::array<ggml_backend_t, 4> backends{};
  std::array<std::thread, 4> threads;
  for (std::size_t index = 0; index < threads.size(); ++index) {
    threads[index] = std::thread(
        [&, index] { backends[index] = ggml_backend_metal_init(); });
  }
  for (auto& thread : threads) thread.join();
  for (auto backend : backends) {
    Expect(backend != nullptr,
           "concurrent Metal context initialization succeeds");
    ggml_backend_free(backend);
  }
  const std::string marker = "using embedded metal library";
  const std::string diagnostics = log.Read();
  const auto first = diagnostics.find(marker);
  Expect(
      first != std::string::npos &&
          diagnostics.find(marker, first + marker.size()) == std::string::npos,
      "the first concurrent contexts compile the shared library once");
  ggml_log_set(nullptr, nullptr);
}
