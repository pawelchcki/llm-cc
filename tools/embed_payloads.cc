#include <sys/stat.h>

#if defined(_WIN32)
#include <io.h>
#define chmod _chmod
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "src/payload_format.h"
#include "tools/sha256.h"

namespace fs = std::filesystem;

namespace {

struct BundleEntry {
  fs::path source;
  std::string destination;
  std::uint64_t size;
  std::array<unsigned char, llmcc::payload::kSha256Size> hash;
};

struct BundleResult {
  std::uint64_t offset;
  std::uint64_t length;
  std::array<unsigned char, llmcc::payload::kSha256Size> hash;
};

using Digest = llmcc::tools::Sha256;

void Write(std::ofstream& output, std::span<const char> bytes,
           Digest* digest = nullptr) {
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw std::runtime_error("cannot write output");
  }
  if (digest != nullptr) {
    digest->Update(bytes);
  }
}

template <typename Integer>
void WriteLittleEndian(std::ofstream& output, Integer value,
                       Digest* digest = nullptr) {
  std::array<char, sizeof(Integer)> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<char>((value >> (index * 8)) & 0xff);
  }
  Write(output, bytes, digest);
}

std::uint64_t CopyFile(const fs::path& source, std::ofstream& output,
                       Digest* digest = nullptr) {
  std::ifstream input(source, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open " + source.string());
  }
  std::array<char, 1024 * 1024> buffer{};
  std::uint64_t copied = 0;
  while (input) {
    input.read(buffer.data(), buffer.size());
    const std::streamsize count = input.gcount();
    if (count > 0) {
      Write(output, std::span<const char>(buffer.data(), count), digest);
      copied += static_cast<std::uint64_t>(count);
    }
  }
  if (!input.eof()) {
    throw std::runtime_error("cannot read " + source.string());
  }
  return copied;
}

std::string Hex(std::span<const unsigned char> bytes) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned char byte : bytes) {
    output << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return output.str();
}

std::string JsonString(std::string_view value) {
  std::ostringstream output;
  output << '"';
  for (const unsigned char byte : value) {
    switch (byte) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\b':
        output << "\\b";
        break;
      case '\f':
        output << "\\f";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        if (byte < 0x20) {
          output << "\\u00" << std::hex << std::setfill('0') << std::setw(2)
                 << static_cast<unsigned int>(byte) << std::dec;
        } else {
          output << static_cast<char>(byte);
        }
    }
  }
  output << '"';
  return output.str();
}

std::array<unsigned char, llmcc::payload::kSha256Size> HashFile(
    const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot hash " + path.string());
  }
  Digest digest;
  std::array<char, 1024 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), buffer.size());
    const std::streamsize count = input.gcount();
    if (count > 0) {
      digest.Update(std::span<const char>(buffer.data(), count));
    }
  }
  if (!input.eof()) {
    throw std::runtime_error("cannot hash " + path.string());
  }
  return digest.Finish();
}

void WriteFooterEntry(
    std::ofstream& output, std::string_view name, std::uint64_t offset,
    std::uint64_t length,
    const std::array<unsigned char, llmcc::payload::kSha256Size>& hash) {
  if (name.size() >= llmcc::payload::kPayloadNameSize) {
    throw std::runtime_error("payload name is too long");
  }
  std::array<char, llmcc::payload::kPayloadNameSize> encoded_name{};
  std::copy(name.begin(), name.end(), encoded_name.begin());
  Write(output, encoded_name);
  WriteLittleEndian(output, offset);
  WriteLittleEndian(output, length);
  Write(output, std::span<const char>(
                    reinterpret_cast<const char*>(hash.data()), hash.size()));
}

bool SafeDestination(std::string_view path) {
  if (path.empty() || path.front() == '/') {
    return false;
  }
  const fs::path parsed(path);
  for (const fs::path& component : parsed) {
    if (component == ".." || component == "." || component.empty()) {
      return false;
    }
  }
  return true;
}

BundleResult WriteBundle(std::ofstream& output, std::span<const char, 8> magic,
                         std::span<const BundleEntry> entries,
                         std::string_view kind) {
  const std::uint64_t offset = static_cast<std::uint64_t>(output.tellp());
  Digest digest;
  Write(output, magic, &digest);
  WriteLittleEndian(output, static_cast<std::uint32_t>(entries.size()),
                    &digest);
  for (const BundleEntry& entry : entries) {
    WriteLittleEndian(
        output, static_cast<std::uint32_t>(entry.destination.size()), &digest);
    WriteLittleEndian(output, static_cast<std::uint32_t>(0600), &digest);
    WriteLittleEndian(output, entry.size, &digest);
    Write(
        output,
        std::span<const char>(reinterpret_cast<const char*>(entry.hash.data()),
                              entry.hash.size()),
        &digest);
    Write(output,
          std::span<const char>(entry.destination.data(),
                                entry.destination.size()),
          &digest);
    const std::uint64_t copied = CopyFile(entry.source, output, &digest);
    if (copied != entry.size) {
      throw std::runtime_error(std::string(kind) +
                               " input changed while packaging");
    }
  }
  return {.offset = offset,
          .length = static_cast<std::uint64_t>(output.tellp()) - offset,
          .hash = digest.Finish()};
}

BundleEntry ParseMapping(std::string_view mapping, std::string_view kind) {
  const std::size_t separator = mapping.find('=');
  if (separator == std::string_view::npos) {
    throw std::runtime_error("invalid " + std::string(kind) + " file mapping");
  }
  const std::string destination(mapping.substr(separator + 1));
  if (!SafeDestination(destination)) {
    throw std::runtime_error("unsafe " + std::string(kind) +
                             " destination: " + destination);
  }
  const fs::path source(mapping.substr(0, separator));
  return {.source = source,
          .destination = destination,
          .size = static_cast<std::uint64_t>(fs::file_size(source)),
          .hash = HashFile(source)};
}

void CheckDuplicateDestinations(std::span<const BundleEntry> entries,
                                std::string_view kind) {
  for (std::size_t left = 0; left < entries.size(); ++left) {
    for (std::size_t right = left + 1; right < entries.size(); ++right) {
      if (entries[left].destination == entries[right].destination) {
        throw std::runtime_error("duplicate " + std::string(kind) +
                                 " destination: " + entries[left].destination);
      }
    }
  }
}

void WriteChecksum(
    const fs::path& checksum_output, const fs::path& output,
    const std::array<unsigned char, llmcc::payload::kSha256Size>& hash) {
  std::ofstream checksum(checksum_output, std::ios::trunc);
  checksum << Hex(hash) << "  " << output.filename().string() << '\n';
  if (!checksum) {
    throw std::runtime_error("cannot write checksum");
  }
}

void WriteManifest(const fs::path& manifest_path, std::string_view name,
                   std::string_view version, std::string_view git_sha,
                   std::string_view llama_commit, std::string_view ggml_abi,
                   std::string_view sha256, std::uint64_t size) {
  std::ofstream manifest(manifest_path, std::ios::trunc);
  manifest << "{\n"
           << "  \"name\": " << JsonString(name) << ",\n"
           << "  \"version\": " << JsonString(version) << ",\n"
           << "  \"git_sha\": " << JsonString(git_sha) << ",\n"
           << "  \"llama_cpp_commit\": " << JsonString(llama_commit) << ",\n"
           << "  \"ggml_backend_api_version\": " << JsonString(ggml_abi)
           << ",\n"
           << "  \"sha256\": " << JsonString(sha256) << ",\n"
           << "  \"size\": " << size << "\n"
           << "}\n";
  if (!manifest) {
    throw std::runtime_error("cannot write manifest");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    fs::path binary;
    fs::path cuda;
    fs::path rocm_module;
    fs::path output;
    fs::path checksum_output;
    fs::path bundle_output;
    fs::path manifest_output;
    std::string name;
    std::string version;
    std::string git_sha;
    std::string llama_commit;
    std::string ggml_abi;
    std::vector<BundleEntry> rocm_entries;
    std::vector<BundleEntry> bundle_entries;
    bool has_binary = false;
    bool has_rocm_file = false;
    bool has_write_bundle = false;
    for (int index = 1; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      auto value = [&]() -> std::string_view {
        if (++index >= argc) {
          throw std::runtime_error("missing value for " +
                                   std::string(argument));
        }
        return argv[index];
      };
      if (argument == "--binary") {
        has_binary = true;
        binary = value();
      } else if (argument == "--cuda") {
        cuda = value();
      } else if (argument == "--rocm-module") {
        rocm_module = value();
      } else if (argument == "--output") {
        output = value();
      } else if (argument == "--checksum-output") {
        checksum_output = value();
      } else if (argument == "--rocm-file") {
        has_rocm_file = true;
        rocm_entries.push_back(ParseMapping(value(), "ROCm"));
      } else if (argument == "--write-bundle") {
        has_write_bundle = true;
        bundle_output = value();
      } else if (argument == "--name") {
        name = value();
      } else if (argument == "--file") {
        bundle_entries.push_back(ParseMapping(value(), "bundle"));
      } else if (argument == "--manifest") {
        manifest_output = value();
      } else if (argument == "--version") {
        version = value();
      } else if (argument == "--git-sha") {
        git_sha = value();
      } else if (argument == "--llama-commit") {
        llama_commit = value();
      } else if (argument == "--ggml-abi") {
        ggml_abi = value();
      } else {
        throw std::runtime_error("unknown argument: " + std::string(argument));
      }
    }

    if (has_binary && has_write_bundle) {
      throw std::runtime_error(
          "--binary and --write-bundle are mutually exclusive");
    }
    if (has_write_bundle) {
      if (bundle_output.empty()) {
        throw std::runtime_error("bundle output path is empty");
      }
      if (!cuda.empty() || !rocm_module.empty() || !output.empty() ||
          has_rocm_file) {
        throw std::runtime_error(
            "fat executable arguments cannot be used with --write-bundle");
      }
      if (name != "cuda" && name != "rocm") {
        throw std::runtime_error("unknown bundle name: " + name);
      }
      CheckDuplicateDestinations(bundle_entries, "bundle");

      std::ofstream bundle(bundle_output, std::ios::binary | std::ios::trunc);
      if (!bundle) {
        throw std::runtime_error("cannot create " + bundle_output.string());
      }
      const std::span<const char, 8> magic = name == "cuda"
                                                 ? llmcc::payload::kCudaMagic
                                                 : llmcc::payload::kRocmMagic;
      const BundleResult result = WriteBundle(
          bundle, magic, std::span<const BundleEntry>(bundle_entries),
          name == "cuda" ? "CUDA" : "ROCm");
      WriteFooterEntry(bundle, name, result.offset, result.length, result.hash);
      bundle.close();
      if (!bundle) {
        throw std::runtime_error("cannot finish " + bundle_output.string());
      }

      const auto bundle_hash = HashFile(bundle_output);
      if (!checksum_output.empty()) {
        WriteChecksum(checksum_output, bundle_output, bundle_hash);
      }
      if (!manifest_output.empty()) {
        WriteManifest(manifest_output, name, version, git_sha, llama_commit,
                      ggml_abi, Hex(bundle_hash),
                      static_cast<std::uint64_t>(fs::file_size(bundle_output)));
      }
      return 0;
    }

    if (!name.empty() || !bundle_entries.empty() || !manifest_output.empty() ||
        !version.empty() || !git_sha.empty() || !llama_commit.empty() ||
        !ggml_abi.empty()) {
      throw std::runtime_error(
          "standalone bundle arguments require --write-bundle");
    }
    if (binary.empty() || cuda.empty() || rocm_module.empty() ||
        output.empty() || checksum_output.empty()) {
      throw std::runtime_error("required payload argument is missing");
    }

    rocm_entries.push_back(
        {.source = rocm_module,
         .destination = "libllm-cc-backend-rocm.so",
         .size = static_cast<std::uint64_t>(fs::file_size(rocm_module)),
         .hash = HashFile(rocm_module)});
    std::sort(rocm_entries.begin(), rocm_entries.end(),
              [](const BundleEntry& left, const BundleEntry& right) {
                return left.destination < right.destination;
              });
    for (std::size_t index = 1; index < rocm_entries.size(); ++index) {
      if (rocm_entries[index - 1].destination ==
          rocm_entries[index].destination) {
        throw std::runtime_error("duplicate ROCm destination: " +
                                 rocm_entries[index].destination);
      }
    }

    std::ofstream combined(output, std::ios::binary | std::ios::trunc);
    if (!combined) {
      throw std::runtime_error("cannot create " + output.string());
    }
    CopyFile(binary, combined);

    const BundleEntry cuda_entry = {
        .source = cuda,
        .destination = "libllm-cc-backend-cuda.so",
        .size = static_cast<std::uint64_t>(fs::file_size(cuda)),
        .hash = HashFile(cuda),
    };
    const BundleResult cuda_bundle =
        WriteBundle(combined, llmcc::payload::kCudaMagic,
                    std::span<const BundleEntry>(&cuda_entry, 1), "CUDA");
    const BundleResult rocm_bundle =
        WriteBundle(combined, llmcc::payload::kRocmMagic,
                    std::span<const BundleEntry>(rocm_entries), "ROCm");

    Write(combined, llmcc::payload::kFooterMagic);
    WriteLittleEndian(combined, llmcc::payload::kFooterVersion);
    WriteLittleEndian(combined, llmcc::payload::kPayloadCount);
    WriteFooterEntry(combined, "cuda", cuda_bundle.offset, cuda_bundle.length,
                     cuda_bundle.hash);
    WriteFooterEntry(combined, "rocm", rocm_bundle.offset, rocm_bundle.length,
                     rocm_bundle.hash);
    combined.close();
    if (!combined) {
      throw std::runtime_error("cannot finish " + output.string());
    }
    if (chmod(output.string().c_str(), 0755) != 0) {
      throw std::runtime_error("cannot make output executable: " +
                               std::string(std::strerror(errno)));
    }

    const auto combined_hash = HashFile(output);
    WriteChecksum(checksum_output, output, combined_hash);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
