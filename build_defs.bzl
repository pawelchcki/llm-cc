"""Small build helpers for project targets."""

def curl_cmake_options(**tls):
    options = {
        "BUILD_CURL_EXE": "OFF",
        "BUILD_EXAMPLES": "OFF",
        "BUILD_LIBCURL_DOCS": "OFF",
        "BUILD_MISC_DOCS": "OFF",
        "BUILD_SHARED_LIBS": "OFF",
        "BUILD_TESTING": "OFF",
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_DISABLE_FIND_PACKAGE_PkgConfig": "ON",
        "CMAKE_INSTALL_LIBDIR": "lib",
        "CURL_BROTLI": "OFF",
        "CURL_DISABLE_LDAP": "ON",
        "CURL_USE_GSSAPI": "OFF",
        "CURL_USE_LIBPSL": "OFF",
        "CURL_USE_LIBSSH2": "OFF",
        "CURL_ZLIB": "OFF",
        "CURL_ZSTD": "OFF",
        "HTTP_ONLY": "ON",
        "USE_LIBIDN2": "OFF",
        "USE_NGHTTP2": "OFF",
        "USE_NGHTTP3": "OFF",
        "USE_NGTCP2": "OFF",
        "USE_QUICHE": "OFF",
    }
    options.update(tls)
    return options

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
  "$runfiles_dir/{workspace}/{version_script}" \\
  "$runfiles_dir/{workspace}/{payload}" "$@"
""".format(
            workspace = workspace,
            script = ctx.file.script.short_path,
            binary = ctx.executable.binary.short_path,
            version_script = ctx.file.version_script.short_path,
            payload = ctx.file.payload.short_path,
        ),
        is_executable = True,
    )
    runfiles = ctx.runfiles(files = [
        ctx.executable.binary,
        ctx.file.script,
        ctx.file.version_script,
        ctx.file.payload,
        ctx.file._version_file,
    ]).merge(ctx.attr.binary[DefaultInfo].default_runfiles)
    return [DefaultInfo(
        executable = launcher,
        runfiles = runfiles,
    )]

install_launcher = rule(
    implementation = _install_launcher_impl,
    executable = True,
    attrs = {
        "binary": attr.label(executable = True, cfg = "target", mandatory = True),
        "script": attr.label(allow_single_file = True, mandatory = True),
        "version_script": attr.label(allow_single_file = True, mandatory = True),
        "payload": attr.label(allow_single_file = True, mandatory = True),
        "_version_file": attr.label(
            allow_single_file = True,
            default = Label("//:VERSION"),
        ),
    },
)

def _payload_archive_impl(ctx):
    output = ctx.actions.declare_file(ctx.attr.output_name)
    binary_files = ctx.attr.binary[DefaultInfo].files.to_list()
    if len(binary_files) != 1:
        fail("payload binary must produce exactly one file")

    mappings = [(binary_files[0], "llm-cc")]
    llama_files = ctx.attr.llama[DefaultInfo].files.to_list()
    libraries = {}
    for source in llama_files:
        if source.basename in [
            "libllama.so",
            "libggml.so",
            "libggml-base.so",
            "libllm-cc-backend-cpu.so",
            "libllm-cc-backend-cuda.so",
            "libllm-cc-backend-rocm.so",
        ]:
            libraries[source.basename] = source

    is_static = ctx.attr.kind == "static" or (ctx.attr.kind == "auto" and not libraries)
    if is_static:
        if libraries:
            fail("the static CPU archive must be built with --config=cpu")
    else:
        required = [
            "libllama.so",
            "libggml.so",
            "libggml-base.so",
            "libllm-cc-backend-cpu.so",
        ]
        if ctx.attr.kind == "universal":
            required += [
                "libllm-cc-backend-cuda.so",
                "libllm-cc-backend-rocm.so",
            ]
        for name in required:
            if name not in libraries:
                fail("%s is missing; build the universal archive with the default Linux x86-64 configuration" % name)
        for name, source in libraries.items():
            mappings.append((source, "lib/" + name))

        if "libllm-cc-backend-cuda.so" in libraries:
            for source in ctx.attr.cuda_runtime[DefaultInfo].files.to_list():
                if ".so" in source.basename:
                    mappings.append((source, "lib/cuda/" + source.basename))
        if "libllm-cc-backend-rocm.so" in libraries:
            for source in ctx.attr.rocm_runtime[DefaultInfo].files.to_list():
                path = source.short_path
                marker = "rocm_sdk/"
                index = path.find(marker)
                relative = path[index + len(marker):] if index >= 0 else source.basename
                if relative.startswith("lib/"):
                    relative = relative[len("lib/"):]
                mappings.append((source, "lib/rocm/" + relative))
            for source in ctx.attr.host_runtime[DefaultInfo].files.to_list():
                mappings.append((source, "lib/rocm/" + source.basename))

    args = ctx.actions.args()
    args.add("--output", output)
    args.add("--root", ctx.attr.root_name)
    if is_static:
        args.add("--require-static")
    for source, destination in mappings:
        args.add("--file", source.path + "=" + destination)
    ctx.actions.run_shell(
        arguments = [args],
        command = "bash %s \"$@\"" % ctx.file._packager.path,
        inputs = depset(
            direct = [ctx.file._packager],
            transitive = [depset([source for source, _ in mappings])],
        ),
        outputs = [output],
        mnemonic = "LlmCcPayloadArchive",
        progress_message = "Creating deterministic payload %{output}",
    )
    return [DefaultInfo(files = depset([output]))]

payload_archive = rule(
    implementation = _payload_archive_impl,
    attrs = {
        "binary": attr.label(mandatory = True),
        "cuda_runtime": attr.label(mandatory = True),
        "host_runtime": attr.label(mandatory = True),
        "kind": attr.string(mandatory = True, values = ["auto", "static", "universal"]),
        "llama": attr.label(mandatory = True),
        "output_name": attr.string(mandatory = True),
        "rocm_runtime": attr.label(mandatory = True),
        "root_name": attr.string(mandatory = True),
        "_packager": attr.label(
            default = Label("//tools:package_payload.sh"),
            allow_single_file = True,
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

def llama_universal_cmake_options():
    """Configuration for the relocatable Linux CPU + CUDA + ROCm package."""
    options = llama_rocm_cmake_options()
    cuda = llama_cuda_cmake_options()
    for key in [
        "CMAKE_CUDA_ARCHITECTURES",
        "CMAKE_CUDA_COMPILER",
        "CMAKE_CUDA_FLAGS",
        "CMAKE_CUDA_HOST_COMPILER",
        "CUDAToolkit_ROOT",
    ]:
        options[key] = cuda[key]
    options.update({
        "BUILD_SHARED_LIBS": "ON",
        "CMAKE_BUILD_RPATH_USE_ORIGIN": "ON",
        "CMAKE_INSTALL_RPATH": "\\$$ORIGIN;\\$$ORIGIN/cuda;\\$$ORIGIN/rocm;\\$$ORIGIN/rocm/llvm/lib;\\$$ORIGIN/rocm/rocm_sysdeps/lib",
        "GGML_BACKEND_DL": "ON",
        "GGML_CUDA": "ON",
        "GGML_HIP": "ON",
    })
    return options

def llama_rocm_cmake_options():
    rocm_root = "$$EXT_BUILD_ROOT/external/+http_archive+rocm_sdk"
    llvm_root = "$$EXT_BUILD_ROOT/external/toolchains_llvm++llvm+llvm_toolchain_llvm"
    gcc_root = "$$EXT_BUILD_ROOT/external/+http_archive+cuda_host_toolchain"
    host_cxx_flags = " ".join([
        "--sysroot=%s/x86_64-buildroot-linux-gnu/sysroot" % gcc_root,
        "-stdlib=libc++",
        "-nostdinc++",
        "-isystem %s/include/c++/v1" % llvm_root,
        "-isystem %s/include/x86_64-unknown-linux-gnu/c++/v1" % llvm_root,
        "-L%s/lib/gcc/x86_64-buildroot-linux-gnu/12.3.0" % gcc_root,
    ])
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
        CMAKE_BUILD_RPATH_USE_ORIGIN = "ON",
        CMAKE_INSTALL_RPATH = "\\$$ORIGIN;\\$$ORIGIN/rocm;\\$$ORIGIN/rocm/llvm/lib;\\$$ORIGIN/rocm/rocm_sysdeps/lib",
        GGML_BACKEND_DL = "ON",
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
        CMAKE_BUILD_RPATH_USE_ORIGIN = "ON",
        CMAKE_INSTALL_RPATH = "\\$$ORIGIN;\\$$ORIGIN/cuda",
        GGML_BACKEND_DL = "ON",
        GGML_CUDA = "ON",
    )
