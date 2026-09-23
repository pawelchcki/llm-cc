#ifndef LLM_CC_BUILD_INFO_H_
#define LLM_CC_BUILD_INFO_H_

#include <string_view>

namespace llmcc::build_info {

// Stamped provenance changes with every commit. Only src/build_info.cc sees
// the generated header, so a new stamp recompiles one small file.
std::string_view Version();
std::string_view GitSha();
std::string_view ArtifactBaseUrl();
std::string_view BackendConfiguration();

}  // namespace llmcc::build_info

#endif  // LLM_CC_BUILD_INFO_H_
