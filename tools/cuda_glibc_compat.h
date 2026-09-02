#ifndef LLM_CC_CUDA_GLIBC_COMPAT_H_
#define LLM_CC_CUDA_GLIBC_COMPAT_H_

// GCC 12's libstdc++ configuration enables these declarations because its
// original sysroot provides them. CUDA only parses the affected timed-lock
// templates; ggml's backend does not instantiate them. Declare the functions
// while compiling against the older portable glibc headers so the templates
// remain well-formed without importing newer glibc symbols. Opaque parameters
// are intentional: NVCC gives Clang a preprocessed source, so including system
// headers again from this forced header would redefine their types.
extern "C" int pthread_cond_clockwait(void* condition, void* mutex, int clock,
                                      const void* timeout);
extern "C" int pthread_mutex_clocklock(void* mutex, int clock,
                                       const void* timeout);

#endif  // LLM_CC_CUDA_GLIBC_COMPAT_H_
