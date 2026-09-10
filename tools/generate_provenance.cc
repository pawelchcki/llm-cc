#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#include "src/sha256.h"

namespace {
std::string Read(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read " + path);
  }
  return {std::istreambuf_iterator<char>(input), {}};
}
void Validate(std::string_view value, std::string_view allowed) {
  if (value.find_first_not_of(allowed) != std::string_view::npos) {
    throw std::runtime_error("invalid provenance value: " + std::string(value));
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 9) {
      throw std::runtime_error("missing provenance arguments");
    }
    std::string version = argv[4];
    std::string commit = argv[5];
    std::string url = argv[6];
    if (*argv[8] != '\0' && version.empty() && commit.empty()) {
      std::ifstream status(argv[8]);
      std::string key;
      std::string value;
      while (status >> key) {
        std::getline(status >> std::ws, value);
        if (key == "STABLE_LLM_CC_VERSION") version = value;
        if (key == "STABLE_LLM_CC_GIT_SHA") commit = value;
        if (key == "STABLE_LLM_CC_ARTIFACT_BASE_URL" && url.empty())
          url = value;
      }
    }
    if (version.empty()) {
      version = Read(argv[3]);
      version.erase(version.find_last_not_of(" \r\n\t") + 1);
      version += "-dev.0+unknown";
      commit.clear();
    }
    if (commit == "unknown") commit.clear();
    if (url == "none") url.clear();
    Validate(
        version,
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.+-");
    Validate(commit, "0123456789abcdef");
    Validate(
        url,
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz:/._~-");
    llmcc::Sha256 hash;
    const std::string configuration =
        std::string("llm-cc-backend-v1\n") + argv[7] + "\n";
    hash.Update(configuration);
    for (int i = 9; i < argc; ++i) {
      const std::string contents = Read(argv[i]);
      // Length-delimited content avoids ambiguous concatenations.
      const std::string length = std::to_string(contents.size()) + ":";
      hash.Update(length);
      hash.Update(contents);
    }
    std::string fingerprint;
    for (auto byte : hash.Finish()) {
      constexpr std::string_view hex = "0123456789abcdef";
      fingerprint += hex[byte >> 4];
      fingerprint += hex[byte & 15];
    }
    std::ofstream header(argv[1]);
    header << "#ifndef LLM_CC_GENERATED_VERSION_H_\n#define "
              "LLM_CC_GENERATED_VERSION_H_\n"
           << "#define LLM_CC_VERSION \"" << version << "\"\n"
           << "#define LLM_CC_GIT_SHA \"" << commit << "\"\n"
           << "#define LLM_CC_ARTIFACT_BASE_URL \"" << url << "\"\n"
           << "#define LLM_CC_BACKEND_CONFIGURATION \"" << fingerprint
           << "\"\n#endif\n";
    std::ofstream metadata(argv[2]);
    metadata << "version='" << version << "'\ngit_sha='" << commit
             << "'\nconfiguration='" << fingerprint << "'\n";
    if (!header || !metadata)
      throw std::runtime_error("cannot write provenance");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
