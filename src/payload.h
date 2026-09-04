#ifndef LLM_CC_PAYLOAD_H_
#define LLM_CC_PAYLOAD_H_

#include <filesystem>
#include <optional>
#include <string_view>

namespace llmcc {

struct PreparedPayload {
  std::filesystem::path path;
  int backing_fd = -1;
};

// Returns nullopt for development binaries without an appended footer.
// Throws when a footer or requested payload is present but invalid.
std::optional<PreparedPayload> PrepareEmbeddedPayload(std::string_view name);

// Explicit-path form used by integration diagnostics to exercise the exact
// shipped executable without changing the production /proc/self/exe path.
std::optional<PreparedPayload> PrepareEmbeddedPayloadFromExecutable(
    const std::filesystem::path& executable, std::string_view name);

// Opens a standalone backend bundle whose archive begins at byte zero. Returns
// nullopt when the file is absent or has a different archive magic, and throws
// when a matching bundle is corrupt.
std::optional<PreparedPayload> PrepareEmbeddedPayloadFromFile(
    const std::filesystem::path& bundle, std::string_view name,
    bool already_verified = false);

// Root shared by extracted runtime payloads and downloaded backend bundles.
std::filesystem::path RuntimeRoot();

}  // namespace llmcc

#endif  // LLM_CC_PAYLOAD_H_
