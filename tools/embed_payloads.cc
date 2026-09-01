#include <sys/stat.h>
#include <zstd.h>

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

std::uint64_t CompressFile(const fs::path& source, const fs::path& output) {
  std::ifstream input(source, std::ios::binary);
  std::ofstream compressed(output, std::ios::binary | std::ios::trunc);
  if (!input || !compressed) {
    throw std::runtime_error("cannot open zstd stream for " + source.string());
  }
  std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context(
      ZSTD_createCCtx(), &ZSTD_freeCCtx);
  if (!context) {
    throw std::runtime_error("cannot create zstd compressor");
  }
  auto set_parameter = [&](ZSTD_cParameter parameter, int value) {
    const std::size_t result =
        ZSTD_CCtx_setParameter(context.get(), parameter, value);
    if (ZSTD_isError(result)) {
      throw std::runtime_error(ZSTD_getErrorName(result));
    }
  };
  set_parameter(ZSTD_c_compressionLevel, 15);
  set_parameter(ZSTD_c_nbWorkers, 0);
  set_parameter(ZSTD_c_enableLongDistanceMatching, 1);
  set_parameter(ZSTD_c_windowLog, 27);
  set_parameter(ZSTD_c_checksumFlag, 1);
  set_parameter(ZSTD_c_contentSizeFlag, 0);
  set_parameter(ZSTD_c_dictIDFlag, 0);

  std::vector<char> input_buffer(ZSTD_CStreamInSize());
  std::vector<char> output_buffer(ZSTD_CStreamOutSize());
  std::uint64_t written = 0;
  bool finished = false;
  while (!finished) {
    input.read(input_buffer.data(), input_buffer.size());
    const std::streamsize count = input.gcount();
    if (count < 0 || (!input && !input.eof())) {
      throw std::runtime_error("cannot read " + source.string());
    }
    ZSTD_inBuffer zstd_input = {input_buffer.data(),
                                static_cast<std::size_t>(count), 0};
    const ZSTD_EndDirective directive =
        input.eof() ? ZSTD_e_end : ZSTD_e_continue;
    do {
      ZSTD_outBuffer zstd_output = {output_buffer.data(), output_buffer.size(),
                                    0};
      const std::size_t remaining = ZSTD_compressStream2(
          context.get(), &zstd_output, &zstd_input, directive);
      if (ZSTD_isError(remaining)) {
        throw std::runtime_error(ZSTD_getErrorName(remaining));
      }
      compressed.write(output_buffer.data(),
                       static_cast<std::streamsize>(zstd_output.pos));
      if (!compressed) {
        throw std::runtime_error("cannot write zstd stream");
      }
      written += zstd_output.pos;
      finished = directive == ZSTD_e_end && remaining == 0;
    } while (zstd_input.pos < zstd_input.size ||
             (directive == ZSTD_e_end && !finished));
  }
  return written;
}

std::string Hex(std::span<const unsigned char> bytes) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned char byte : bytes) {
    output << std::setw(2) << static_cast<unsigned int>(byte);
  }
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
                         const fs::path& compressed_entry,
                         std::string_view kind) {
  const std::uint64_t offset = static_cast<std::uint64_t>(output.tellp());
  Digest digest;
  Write(output, magic, &digest);
  WriteLittleEndian(output, static_cast<std::uint32_t>(entries.size()),
                    &digest);
  for (const BundleEntry& entry : entries) {
    const std::uint64_t compressed_size =
        CompressFile(entry.source, compressed_entry);
    WriteLittleEndian(
        output, static_cast<std::uint32_t>(entry.destination.size()), &digest);
    WriteLittleEndian(output, static_cast<std::uint32_t>(0600), &digest);
    WriteLittleEndian(output, entry.size, &digest);
    WriteLittleEndian(output, compressed_size, &digest);
    Write(
        output,
        std::span<const char>(reinterpret_cast<const char*>(entry.hash.data()),
                              entry.hash.size()),
        &digest);
    Write(output,
          std::span<const char>(entry.destination.data(),
                                entry.destination.size()),
          &digest);
    const std::uint64_t copied = CopyFile(compressed_entry, output, &digest);
    if (copied != compressed_size) {
      throw std::runtime_error("compressed " + std::string(kind) +
                               " input changed while packaging");
    }
  }
  return {.offset = offset,
          .length = static_cast<std::uint64_t>(output.tellp()) - offset,
          .hash = digest.Finish()};
}

}  // namespace

int main(int argc, char** argv) {
  try {
    fs::path binary;
    fs::path cuda;
    fs::path rocm_module;
    fs::path output;
    fs::path checksum_output;
    std::vector<BundleEntry> rocm_entries;
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
        const std::string mapping(value());
        const std::size_t separator = mapping.find('=');
        if (separator == std::string::npos) {
          throw std::runtime_error("invalid ROCm file mapping");
        }
        const std::string destination = mapping.substr(separator + 1);
        if (!SafeDestination(destination)) {
          throw std::runtime_error("unsafe ROCm destination: " + destination);
        }
        const fs::path source = mapping.substr(0, separator);
        rocm_entries.push_back(
            {.source = source,
             .destination = destination,
             .size = static_cast<std::uint64_t>(fs::file_size(source)),
             .hash = HashFile(source)});
      } else {
        throw std::runtime_error("unknown argument: " + std::string(argument));
      }
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

    const fs::path compressed_entry = output.string() + ".zstd.tmp";
    const BundleEntry cuda_entry = {
        .source = cuda,
        .destination = "libllm-cc-backend-cuda.so",
        .size = static_cast<std::uint64_t>(fs::file_size(cuda)),
        .hash = HashFile(cuda),
    };
    const BundleResult cuda_bundle = WriteBundle(
        combined, llmcc::payload::kCudaMagic,
        std::span<const BundleEntry>(&cuda_entry, 1), compressed_entry, "CUDA");
    const BundleResult rocm_bundle = WriteBundle(
        combined, llmcc::payload::kRocmMagic,
        std::span<const BundleEntry>(rocm_entries), compressed_entry, "ROCm");
    std::error_code remove_error;
    fs::remove(compressed_entry, remove_error);

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
    if (chmod(output.c_str(), 0755) != 0) {
      throw std::runtime_error("cannot make output executable: " +
                               std::string(std::strerror(errno)));
    }

    const auto combined_hash = HashFile(output);
    std::ofstream checksum(checksum_output, std::ios::trunc);
    checksum << Hex(combined_hash) << "  " << output.filename().string()
             << '\n';
    if (!checksum) {
      throw std::runtime_error("cannot write checksum");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
