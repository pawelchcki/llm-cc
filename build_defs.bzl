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
  "$runfiles_dir/{workspace}/{version}" "$@"
""".format(
            workspace = workspace,
            script = ctx.file.script.short_path,
            binary = ctx.executable.binary.short_path,
            version = ctx.file.version.short_path,
        ),
        is_executable = True,
    )
    return [DefaultInfo(
        executable = launcher,
        runfiles = ctx.runfiles(files = [
            ctx.executable.binary,
            ctx.file.script,
            ctx.file.version,
        ]),
    )]

install_launcher = rule(
    implementation = _install_launcher_impl,
    executable = True,
    attrs = {
        "binary": attr.label(executable = True, cfg = "target", mandatory = True),
        "script": attr.label(allow_single_file = True, mandatory = True),
        "version": attr.label(allow_single_file = True, mandatory = True),
    },
)

def llama_cmake_options(**backend):
    options = {
        "BUILD_SHARED_LIBS": "OFF",
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_INSTALL_LIBDIR": "lib",
        "GGML_BACKEND_DL": "OFF",
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
    return llama_cmake_options(
        BUILD_SHARED_LIBS = "ON",
        CMAKE_HIP_COMPILER = "$$HIPCXX$$",
        GGML_HIP = "ON",
        GPU_TARGETS = "$$GPU_TARGETS$$",
    )
