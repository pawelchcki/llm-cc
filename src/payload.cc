#include "src/payload.h"

#if defined(__linux__)

#include <fcntl.h>
#include <linux/memfd.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/payload_format.h"

#ifndef F_ADD_SEALS
// Stable Linux UAPI values, absent from the older glibc headers in the
// portability sysroot even though the kernel ABI supports them.
#define F_ADD_SEALS 1033
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008
#endif

namespace fs = std::filesystem;

namespace llmcc {
namespace {

struct PayloadLocation {
  std::uint64_t offset;
  std::uint64_t length;
  std::array<unsigned char, payload::kSha256Size> hash;
};

struct ArchiveEntry {
  std::string path;
  std::uint32_t mode;
  std::uint64_t size;
  std::uint64_t compressed_size;
  std::array<unsigned char, payload::kSha256Size> hash;
  std::uint64_t data_offset;
};

class Digest {
 public:
  Digest() : context_(EVP_MD_CTX_new(), &EVP_MD_CTX_free) {
    if (!context_ ||
        EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1) {
      throw std::runtime_error("cannot initialize SHA-256");
    }
  }

  void Update(std::span<const char> bytes) {
    if (EVP_DigestUpdate(context_.get(), bytes.data(), bytes.size()) != 1) {
      throw std::runtime_error("cannot update SHA-256");
    }
  }

  std::array<unsigned char, payload::kSha256Size> Finish() {
    std::array<unsigned char, payload::kSha256Size> result{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context_.get(), result.data(), &size) != 1 ||
        size != result.size()) {
      throw std::runtime_error("cannot finish SHA-256");
    }
    return result;
  }

 private:
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
};

class FileDescriptor {
 public:
  explicit FileDescriptor(int fd = -1) : fd_(fd) {}
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  FileDescriptor(FileDescriptor&& other) noexcept
      : fd_(std::exchange(other.fd_, -1)) {}
  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
      Reset();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  ~FileDescriptor() { Reset(); }

  int get() const { return fd_; }
  int Release() { return std::exchange(fd_, -1); }

 private:
  void Reset() {
    if (fd_ >= 0) {
      close(fd_);
    }
    fd_ = -1;
  }
  int fd_;
};

void ReadExact(int fd, std::uint64_t offset, std::span<char> output) {
  std::size_t completed = 0;
  while (completed < output.size()) {
    const ssize_t count =
        pread(fd, output.data() + completed, output.size() - completed,
              static_cast<off_t>(offset + completed));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      throw std::runtime_error(
          "cannot read embedded payload: " +
          std::string(count == 0 ? "unexpected EOF" : std::strerror(errno)));
    }
    completed += static_cast<std::size_t>(count);
  }
}

void WriteExact(int fd, std::span<const char> input) {
  std::size_t completed = 0;
  while (completed < input.size()) {
    const ssize_t count =
        write(fd, input.data() + completed, input.size() - completed);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      throw std::runtime_error("cannot write runtime payload: " +
                               std::string(std::strerror(errno)));
    }
    completed += static_cast<std::size_t>(count);
  }
}

template <typename Integer>
Integer ReadLittleEndian(std::span<const char> input, std::size_t offset) {
  if (offset > input.size() || input.size() - offset < sizeof(Integer)) {
    throw std::runtime_error("truncated embedded payload metadata");
  }
  Integer value = 0;
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    value |=
        static_cast<Integer>(static_cast<unsigned char>(input[offset + index]))
        << (index * 8);
  }
  return value;
}

std::string Hex(std::span<const unsigned char> bytes) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned char byte : bytes) {
    output << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return output.str();
}

std::array<unsigned char, payload::kSha256Size> HashRange(
    int fd, std::uint64_t offset, std::uint64_t length) {
  Digest digest;
  std::array<char, 1024 * 1024> buffer{};
  std::uint64_t completed = 0;
  while (completed < length) {
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer.size(), length - completed));
    ReadExact(fd, offset + completed, std::span<char>(buffer.data(), count));
    digest.Update(std::span<const char>(buffer.data(), count));
    completed += count;
  }
  return digest.Finish();
}

std::array<unsigned char, payload::kSha256Size> HashFile(const fs::path& path) {
  FileDescriptor fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (fd.get() < 0) {
    throw std::runtime_error("cannot open cached runtime file " +
                             path.string() + ": " + std::strerror(errno));
  }
  struct stat status{};
  if (fstat(fd.get(), &status) != 0 || !S_ISREG(status.st_mode)) {
    throw std::runtime_error("cached runtime path is not a regular file: " +
                             path.string());
  }
  return HashRange(fd.get(), 0, static_cast<std::uint64_t>(status.st_size));
}

std::optional<PayloadLocation> FindPayload(int fd, std::string_view name) {
  struct stat status{};
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
    throw std::runtime_error("cannot inspect /proc/self/exe");
  }
  const std::uint64_t size = static_cast<std::uint64_t>(status.st_size);
  if (size < payload::kFooterSize) {
    return std::nullopt;
  }
  std::array<char, payload::kFooterSize> footer{};
  const std::uint64_t footer_offset = size - footer.size();
  ReadExact(fd, footer_offset, footer);
  if (!std::equal(payload::kFooterMagic.begin(), payload::kFooterMagic.end(),
                  footer.begin())) {
    return std::nullopt;
  }
  if (ReadLittleEndian<std::uint32_t>(footer, 8) != payload::kFooterVersion ||
      ReadLittleEndian<std::uint32_t>(footer, 12) != payload::kPayloadCount) {
    throw std::runtime_error("unsupported embedded payload footer");
  }
  for (std::uint32_t index = 0; index < payload::kPayloadCount; ++index) {
    const std::size_t entry =
        payload::kFooterHeaderSize + index * payload::kFooterEntrySize;
    const char* name_begin = footer.data() + entry;
    const std::size_t name_length =
        strnlen(name_begin, payload::kPayloadNameSize);
    const std::string_view encoded_name(name_begin, name_length);
    const std::uint64_t offset =
        ReadLittleEndian<std::uint64_t>(footer, entry + 16);
    const std::uint64_t length =
        ReadLittleEndian<std::uint64_t>(footer, entry + 24);
    if (offset > footer_offset || length > footer_offset - offset) {
      throw std::runtime_error("embedded payload range is invalid");
    }
    if (encoded_name == name) {
      PayloadLocation location{.offset = offset, .length = length};
      std::memcpy(location.hash.data(), footer.data() + entry + 32,
                  location.hash.size());
      return location;
    }
  }
  throw std::runtime_error("embedded payload is missing: " + std::string(name));
}

bool SafeRelativePath(std::string_view value) {
  if (value.empty() || value.front() == '/') {
    return false;
  }
  for (const fs::path& component : fs::path(value)) {
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
  }
  return true;
}

std::vector<ArchiveEntry> ReadArchiveEntries(int fd,
                                             const PayloadLocation& payload,
                                             std::span<const char, 8> magic,
                                             std::string_view kind) {
  std::array<char, 12> header{};
  if (payload.length < header.size()) {
    throw std::runtime_error("truncated " + std::string(kind) + " payload");
  }
  ReadExact(fd, payload.offset, header);
  if (!std::equal(magic.begin(), magic.end(), header.begin())) {
    throw std::runtime_error("invalid " + std::string(kind) + " payload magic");
  }
  const std::uint32_t count = ReadLittleEndian<std::uint32_t>(header, 8);
  if (count == 0 || count > 100000) {
    throw std::runtime_error("invalid " + std::string(kind) +
                             " payload entry count");
  }
  std::uint64_t cursor = header.size();
  std::vector<ArchiveEntry> entries;
  entries.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    constexpr std::size_t kEntryHeaderSize = 56;
    std::array<char, kEntryHeaderSize> entry_header{};
    if (cursor > payload.length ||
        payload.length - cursor < entry_header.size()) {
      throw std::runtime_error("truncated " + std::string(kind) +
                               " payload entry");
    }
    ReadExact(fd, payload.offset + cursor, entry_header);
    cursor += entry_header.size();
    const std::uint32_t path_size =
        ReadLittleEndian<std::uint32_t>(entry_header, 0);
    const std::uint32_t mode = ReadLittleEndian<std::uint32_t>(entry_header, 4);
    const std::uint64_t file_size =
        ReadLittleEndian<std::uint64_t>(entry_header, 8);
    const std::uint64_t compressed_size =
        ReadLittleEndian<std::uint64_t>(entry_header, 16);
    if (path_size == 0 || path_size > 4096 || cursor > payload.length ||
        path_size > payload.length - cursor || compressed_size == 0) {
      throw std::runtime_error("invalid " + std::string(kind) +
                               " payload path");
    }
    std::string path(path_size, '\0');
    ReadExact(fd, payload.offset + cursor, path);
    cursor += path_size;
    if (!SafeRelativePath(path) || cursor > payload.length ||
        compressed_size > payload.length - cursor) {
      throw std::runtime_error("unsafe or truncated " + std::string(kind) +
                               " payload entry");
    }
    ArchiveEntry entry{.path = std::move(path),
                       .mode = mode,
                       .size = file_size,
                       .compressed_size = compressed_size,
                       .data_offset = payload.offset + cursor};
    std::memcpy(entry.hash.data(), entry_header.data() + 24, entry.hash.size());
    entries.push_back(std::move(entry));
    cursor += compressed_size;
  }
  if (cursor != payload.length) {
    throw std::runtime_error(std::string(kind) + " payload has trailing data");
  }
  return entries;
}

fs::path RuntimeRoot() {
  if (const char* override = std::getenv("LLM_CC_RUNTIME_DIR");
      override != nullptr && *override != '\0') {
    return override;
  }
  if (const char* xdg = std::getenv("XDG_CACHE_HOME");
      xdg != nullptr && *xdg != '\0') {
    return fs::path(xdg) / "llm-cc" / "runtime";
  }
  if (const char* home = std::getenv("HOME");
      home != nullptr && *home != '\0') {
    return fs::path(home) / ".cache" / "llm-cc" / "runtime";
  }
  throw std::runtime_error(
      "HOME is unset; set LLM_CC_RUNTIME_DIR for the ROCm runtime cache");
}

bool CacheIsValid(const fs::path& directory,
                  std::span<const ArchiveEntry> entries,
                  std::string_view payload_hash) {
  std::ifstream marker(directory / ".complete");
  std::string recorded;
  if (!(marker >> recorded) || recorded != payload_hash) {
    return false;
  }
  try {
    for (const ArchiveEntry& entry : entries) {
      const fs::path path = directory / entry.path;
      if (fs::file_size(path) != entry.size || HashFile(path) != entry.hash) {
        return false;
      }
    }
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

void DecompressEntry(int executable_fd, const ArchiveEntry& entry,
                     int output_fd, std::string_view kind) {
  Digest digest;
  std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context(
      ZSTD_createDCtx(), &ZSTD_freeDCtx);
  if (!context) {
    throw std::runtime_error("cannot create zstd decompressor");
  }
  std::vector<char> compressed(ZSTD_DStreamInSize());
  std::vector<char> decompressed(ZSTD_DStreamOutSize());
  std::uint64_t consumed = 0;
  std::uint64_t produced = 0;
  std::size_t frame_remaining = 1;
  while (consumed < entry.compressed_size) {
    const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(
        compressed.size(), entry.compressed_size - consumed));
    ReadExact(executable_fd, entry.data_offset + consumed,
              std::span<char>(compressed.data(), count));
    consumed += count;
    ZSTD_inBuffer input = {compressed.data(), count, 0};
    while (input.pos < input.size) {
      ZSTD_outBuffer zstd_output = {decompressed.data(), decompressed.size(),
                                    0};
      frame_remaining =
          ZSTD_decompressStream(context.get(), &zstd_output, &input);
      if (ZSTD_isError(frame_remaining)) {
        throw std::runtime_error(ZSTD_getErrorName(frame_remaining));
      }
      if (zstd_output.pos > entry.size - produced) {
        throw std::runtime_error(std::string(kind) +
                                 " entry expands past its size");
      }
      const std::span<const char> bytes(decompressed.data(), zstd_output.pos);
      digest.Update(bytes);
      WriteExact(output_fd, bytes);
      produced += zstd_output.pos;
    }
  }
  if (frame_remaining != 0 || produced != entry.size ||
      digest.Finish() != entry.hash) {
    throw std::runtime_error(std::string(kind) +
                             " entry failed validation: " + entry.path);
  }
}

void ExtractEntry(int executable_fd, const ArchiveEntry& entry,
                  const fs::path& root) {
  const fs::path destination = root / entry.path;
  fs::create_directories(destination.parent_path());
  chmod(destination.parent_path().c_str(), 0700);
  FileDescriptor output(open(
      destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
      entry.mode & 0777));
  if (output.get() < 0) {
    throw std::runtime_error("cannot create " + destination.string() + ": " +
                             std::strerror(errno));
  }
  DecompressEntry(executable_fd, entry, output.get(), "ROCm runtime");
  if (fsync(output.get()) != 0) {
    throw std::runtime_error("cannot sync ROCm runtime entry: " + entry.path);
  }
}

PreparedPayload PrepareCuda(int executable_fd,
                            const PayloadLocation& location) {
  if (HashRange(executable_fd, location.offset, location.length) !=
      location.hash) {
    throw std::runtime_error("CUDA payload SHA-256 mismatch");
  }
  const std::vector<ArchiveEntry> entries =
      ReadArchiveEntries(executable_fd, location, payload::kCudaMagic, "CUDA");
  if (entries.size() != 1 ||
      entries.front().path != "libllm-cc-backend-cuda.so") {
    throw std::runtime_error("CUDA payload has an unexpected file layout");
  }
  const ArchiveEntry& entry = entries.front();
  const int raw_fd = static_cast<int>(syscall(SYS_memfd_create, "llm-cc-cuda",
                                              MFD_CLOEXEC | MFD_ALLOW_SEALING));
  if (raw_fd < 0) {
    throw std::runtime_error("cannot create CUDA memfd: " +
                             std::string(std::strerror(errno)));
  }
  FileDescriptor memfd(raw_fd);
  if (ftruncate(memfd.get(), static_cast<off_t>(entry.size)) != 0) {
    throw std::runtime_error("cannot size CUDA memfd");
  }
  DecompressEntry(executable_fd, entry, memfd.get(), "CUDA");
  if (fcntl(memfd.get(), F_ADD_SEALS,
            F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) != 0) {
    throw std::runtime_error("CUDA payload sealing failed");
  }
  const int retained_fd = memfd.Release();
  return {.path = "/proc/self/fd/" + std::to_string(retained_fd),
          .backing_fd = retained_fd};
}

PreparedPayload PrepareRocm(int executable_fd,
                            const PayloadLocation& location) {
  if (HashRange(executable_fd, location.offset, location.length) !=
      location.hash) {
    throw std::runtime_error("ROCm payload SHA-256 mismatch");
  }
  const std::vector<ArchiveEntry> entries =
      ReadArchiveEntries(executable_fd, location, payload::kRocmMagic, "ROCm");
  const std::string hash = Hex(location.hash);
  const fs::path root = RuntimeRoot();
  fs::create_directories(root);
  chmod(root.c_str(), 0700);
  const fs::path final = root / hash;
  if (!CacheIsValid(final, entries, hash)) {
    if (fs::exists(final)) {
      const fs::path corrupt =
          root / ("." + hash + ".corrupt." + std::to_string(getpid()));
      std::error_code error;
      fs::rename(final, corrupt, error);
      if (!error) {
        fs::remove_all(corrupt, error);
      } else if (!CacheIsValid(final, entries, hash)) {
        throw std::runtime_error("cannot replace corrupted ROCm runtime cache");
      }
    }
    const fs::path staging =
        root / ("." + hash + ".tmp." + std::to_string(getpid()));
    std::error_code error;
    fs::remove_all(staging, error);
    if (!fs::create_directory(staging) || chmod(staging.c_str(), 0700) != 0) {
      throw std::runtime_error("cannot create ROCm runtime staging directory");
    }
    try {
      for (const ArchiveEntry& entry : entries) {
        ExtractEntry(executable_fd, entry, staging);
      }
      {
        std::ofstream marker(staging / ".complete", std::ios::trunc);
        marker << hash << '\n';
        if (!marker) {
          throw std::runtime_error("cannot write ROCm cache marker");
        }
      }
      chmod((staging / ".complete").c_str(), 0600);
      fs::rename(staging, final, error);
      if (error && !CacheIsValid(final, entries, hash)) {
        throw std::runtime_error("cannot publish ROCm runtime cache: " +
                                 error.message());
      }
      if (error) {
        fs::remove_all(staging, error);
      }
    } catch (...) {
      fs::remove_all(staging, error);
      throw;
    }
  }
  return {.path = final / "libllm-cc-backend-rocm.so"};
}

}  // namespace

std::optional<PreparedPayload> PrepareEmbeddedPayloadFromExecutable(
    const fs::path& executable_path, std::string_view name) {
  FileDescriptor executable(
      open(executable_path.c_str(), O_RDONLY | O_CLOEXEC));
  if (executable.get() < 0) {
    return std::nullopt;
  }
  const std::optional<PayloadLocation> location =
      FindPayload(executable.get(), name);
  if (!location.has_value()) {
    return std::nullopt;
  }
  if (name == "cuda") {
    return PrepareCuda(executable.get(), *location);
  }
  if (name == "rocm") {
    return PrepareRocm(executable.get(), *location);
  }
  throw std::runtime_error("unknown embedded payload: " + std::string(name));
}

std::optional<PreparedPayload> PrepareEmbeddedPayload(std::string_view name) {
  return PrepareEmbeddedPayloadFromExecutable("/proc/self/exe", name);
}

}  // namespace llmcc

#else

namespace llmcc {

std::optional<PreparedPayload> PrepareEmbeddedPayload(std::string_view) {
  return std::nullopt;
}

std::optional<PreparedPayload> PrepareEmbeddedPayloadFromExecutable(
    const std::filesystem::path&, std::string_view) {
  return std::nullopt;
}

}  // namespace llmcc

#endif
