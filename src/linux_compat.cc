#if defined(__linux__)

#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

// libc++ uses copy_file_range when it is available and falls back to a normal
// copy when the syscall returns ENOSYS. Calling the stable kernel ABI directly
// avoids importing the glibc 2.27 wrapper into the portable executable.
extern "C" ssize_t copy_file_range(int input, off_t* input_offset, int output,
                                   off_t* output_offset, size_t length,
                                   unsigned int flags) {
  return syscall(SYS_copy_file_range, input, input_offset, output,
                 output_offset, length, flags);
}

#endif
