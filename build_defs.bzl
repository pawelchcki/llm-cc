"""Small build helpers for project targets."""

def _install_launcher_impl(ctx):
    launcher = ctx.actions.declare_file(ctx.label.name)
    workspace = ctx.workspace_name
    ctx.actions.write(
        output = launcher,
        content = """#!/usr/bin/env bash
set -euo pipefail
runfiles_dir="${{RUNFILES_DIR:-$0.runfiles}}"
exec "$runfiles_dir/{workspace}/{script}" \\
  "$runfiles_dir/{workspace}/{binary}" \\
  "$runfiles_dir/{workspace}/{version_script}" "$@"
""".format(
            workspace = workspace,
            script = ctx.file.script.short_path,
            binary = ctx.executable.binary.short_path,
            version_script = ctx.file.version_script.short_path,
        ),
        is_executable = True,
    )
    return [DefaultInfo(
        executable = launcher,
        runfiles = ctx.runfiles(files = [
            ctx.executable.binary,
            ctx.file.script,
            ctx.file.version_script,
            ctx.file._version_file,
        ]),
    )]

install_launcher = rule(
    implementation = _install_launcher_impl,
    executable = True,
    attrs = {
        "binary": attr.label(executable = True, cfg = "target", mandatory = True),
        "script": attr.label(allow_single_file = True, mandatory = True),
        "version_script": attr.label(allow_single_file = True, mandatory = True),
        "_version_file": attr.label(
            allow_single_file = True,
            default = Label("//:VERSION"),
        ),
    },
)

def llama_cmake_options(**backend):
    options = {
        "BUILD_SHARED_LIBS": "OFF",
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_INSTALL_LIBDIR": "lib",
        "CMAKE_PLATFORM_NO_VERSIONED_SONAME": "ON",
        "GGML_BACKEND_DL": "OFF",
        "GGML_CCACHE": "OFF",
        "GGML_CUDA": "OFF",
        "GGML_HIP": "OFF",
        "GGML_METAL": "OFF",
        "GGML_NATIVE": "OFF",
        "GGML_OPENMP": "OFF",
        "LLAMA_BUILD_APP": "OFF",
        "LLAMA_BUILD_COMMON": "OFF",
        "LLAMA_BUILD_EXAMPLES": "OFF",
        "LLAMA_BUILD_SERVER": "OFF",
        "LLAMA_BUILD_TESTS": "OFF",
        "LLAMA_BUILD_TOOLS": "OFF",
    }
    options.update(backend)
    return options

def llama_rocm_cmake_options():
    rocm_root = "$$EXT_BUILD_ROOT/external/+http_archive+rocm_sdk"
    llvm_root = "$$EXT_BUILD_ROOT/external/toolchains_llvm++llvm+llvm_toolchain_llvm"
    gcc_root = "$$EXT_BUILD_ROOT/external/+http_archive+cuda_host_toolchain"
    host_cxx_flags = "-stdlib=libc++ -nostdinc++ -isystem %s/include/c++/v1 -isystem %s/include/x86_64-unknown-linux-gnu/c++/v1 -L%s/lib/gcc/x86_64-buildroot-linux-gnu/12.3.0" % (llvm_root, llvm_root, gcc_root)
    return llama_cmake_options(
        BUILD_SHARED_LIBS = "ON",
        CMAKE_HIP_COMPILER = "$(execpath @rocm_sdk//:clang)",
        CMAKE_HIP_COMPILER_ID = "Clang",
        CMAKE_HIP_COMPILER_ID_RUN = "ON",
        CMAKE_HIP_COMPILER_ROCM_ROOT = rocm_root,
        CMAKE_HIP_COMPILER_VERSION = "23.0.0",
        CMAKE_HIP_FLAGS = "--rocm-path=%s -frandom-seed=llm-cc -fuse-cuid=none -ffile-prefix-map=$$EXT_BUILD_ROOT=. -fdebug-prefix-map=$$EXT_BUILD_ROOT=. %s" % (rocm_root, host_cxx_flags),
        CMAKE_HIP_PLATFORM = "amd",
        CMAKE_PREFIX_PATH = rocm_root,
        GGML_HIP = "ON",
        GPU_TARGETS = "gfx1100;gfx1101;gfx1102",
    )

def llama_cuda_cmake_options():
    cuda_root = "$$EXT_BUILD_ROOT/external/+cuda_sdk_repository+cuda_sdk/sdk"
    return llama_cmake_options(
        BUILD_SHARED_LIBS = "ON",
        CMAKE_CUDA_ARCHITECTURES = "75-virtual;80-virtual;86-real;89-real;90-virtual;120a-real",
        CMAKE_CUDA_COMPILER = "$(execpath @cuda_sdk//:nvcc)",
        CMAKE_CUDA_FLAGS = "--allow-unsupported-compiler --frandom-seed=llm-cc -Xcompiler=-ffile-prefix-map=$$EXT_BUILD_ROOT=. -Xcompiler=-fdebug-prefix-map=$$EXT_BUILD_ROOT=.",
        CMAKE_CUDA_HOST_COMPILER = "$(execpath //tools:cuda_host_compiler_wrapper.sh)",
        CUDAToolkit_ROOT = cuda_root,
        GGML_CUDA = "ON",
    )
