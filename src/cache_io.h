#ifndef LLM_CC_CACHE_IO_H_
#define LLM_CC_CACHE_IO_H_

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace llmcc {
namespace cache_io {

inline void CheckNotSymlink(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::system_error(error, "cannot inspect " + path.string());
  }
  if (!error && std::filesystem::is_symlink(status)) {
    throw std::runtime_error("refusing symbolic link " + path.string());
  }
#if defined(_WIN32)
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    throw std::runtime_error("refusing reparse point " + path.string());
  }
#endif
}

inline void EnsurePrivateDirectory(const std::filesystem::path& path) {
  CheckNotSymlink(path);
  std::filesystem::create_directories(path);
  CheckNotSymlink(path);
#if !defined(_WIN32)
  std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
#endif
}

inline std::string UniqueSuffix() {
  static std::atomic<std::uint64_t> sequence{0};
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  return ".tmp." +
         std::to_string(
#if defined(_WIN32)
             static_cast<unsigned long long>(GetCurrentProcessId())
#else
             static_cast<unsigned long long>(getpid())
#endif
                 ) +
         "." + std::to_string(tick) + "." +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

class FileLock {
 public:
  explicit FileLock(const std::filesystem::path& path) {
    if (!path.parent_path().empty()) {
      std::filesystem::create_directories(path.parent_path());
    }
    CheckNotSymlink(path);
#if defined(_WIN32)
    handle_ = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      throw std::system_error(static_cast<int>(GetLastError()),
                              std::system_category(),
                              "cannot lock " + path.string());
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle_, FileAttributeTagInfo,
                                      &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes &
         (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      throw std::runtime_error("invalid lock file " + path.string());
    }
    OVERLAPPED overlap{};
    if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD,
                    &overlap)) {
      const int error = static_cast<int>(GetLastError());
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      throw std::system_error(error, std::system_category(),
                              "cannot lock " + path.string());
    }
#else
    fd_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat details{};
    if (fd_ < 0 || fstat(fd_, &details) != 0) {
      const int error = errno;
      if (fd_ >= 0) close(fd_);
      throw std::system_error(error, std::system_category(),
                              "cannot lock " + path.string());
    }
    if (!S_ISREG(details.st_mode)) {
      close(fd_);
      throw std::runtime_error("invalid lock file " + path.string());
    }
    int result;
    do {
      result = flock(fd_, LOCK_EX);
    } while (result != 0 && errno == EINTR);
    if (result != 0 || fchmod(fd_, 0600) != 0) {
      const int error = errno;
      close(fd_);
      throw std::system_error(error, std::system_category(),
                              "cannot lock " + path.string());
    }
#endif
  }
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  ~FileLock() {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) {
      OVERLAPPED overlap{};
      UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlap);
      CloseHandle(handle_);
    }
#else
    if (fd_ >= 0) {
      flock(fd_, LOCK_UN);
      close(fd_);
    }
#endif
  }

 private:
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};

inline void WritePrivateFile(const std::filesystem::path& path,
                             std::string_view bytes) {
#if defined(_WIN32)
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(),
                            "cannot create " + path.string());
  DWORD written = 0;
  const bool ok =
      WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()),
                &written, nullptr) &&
      written == bytes.size() && FlushFileBuffers(handle);
  CloseHandle(handle);
  if (!ok) throw std::runtime_error("cannot write " + path.string());
#else
  const int fd = open(
      path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    throw std::system_error(errno, std::system_category(),
                            "cannot create " + path.string());
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written =
        write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      const int error = errno;
      close(fd);
      throw std::system_error(error, std::system_category(),
                              "cannot write " + path.string());
    }
    offset += static_cast<std::size_t>(written);
  }
  if (fsync(fd) != 0) {
    const int error = errno;
    close(fd);
    throw std::system_error(error, std::system_category(),
                            "cannot sync " + path.string());
  }
  close(fd);
#endif
}

inline void AtomicWriteFile(const std::filesystem::path& target,
                            std::string_view bytes) {
  if (!target.parent_path().empty()) {
    std::filesystem::create_directories(target.parent_path());
  }
  CheckNotSymlink(target);
  std::filesystem::path temporary;
  for (int attempt = 0; attempt != 10; ++attempt) {
    temporary = target;
    temporary += std::filesystem::path(UniqueSuffix());
    try {
      WritePrivateFile(temporary, bytes);
      break;
    } catch (const std::system_error& error) {
      if (error.code() == std::errc::file_exists) {
        if (attempt == 9) throw;
        continue;  // This temporary belongs to another writer.
      }
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      throw;
    } catch (...) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      throw;
    }
  }
#if defined(_WIN32)
  if (!MoveFileExW(temporary.c_str(), target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const auto error = GetLastError();
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw std::system_error(static_cast<int>(error), std::system_category(),
                            "cannot replace " + target.string());
  }
#else
  struct stat information{};
  if (lstat(target.c_str(), &information) == 0 &&
      S_ISLNK(information.st_mode)) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw std::runtime_error("refusing to replace symbolic link " +
                             target.string());
  }
  if (rename(temporary.c_str(), target.c_str()) != 0) {
    const int error = errno;
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw std::system_error(error, std::system_category(),
                            "cannot replace " + target.string());
  }
#endif
}

}  // namespace cache_io
}  // namespace llmcc

#endif  // LLM_CC_CACHE_IO_H_
