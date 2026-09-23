#include "src/build_info.h"

#include "generated/version.h"

namespace llmcc::build_info {

std::string_view Version() {
  return {LLM_CC_VERSION, sizeof(LLM_CC_VERSION) - 1};
}

std::string_view GitSha() {
  return {LLM_CC_GIT_SHA, sizeof(LLM_CC_GIT_SHA) - 1};
}

std::string_view ArtifactBaseUrl() {
  return {LLM_CC_ARTIFACT_BASE_URL, sizeof(LLM_CC_ARTIFACT_BASE_URL) - 1};
}

std::string_view BackendConfiguration() {
  return {LLM_CC_BACKEND_CONFIGURATION,
          sizeof(LLM_CC_BACKEND_CONFIGURATION) - 1};
}

}  // namespace llmcc::build_info
