"""Small build helpers for project targets."""

load("//tools:provenance.bzl", "ProvenanceInfo")

# Single source of truth for both llama.cpp's GPU_TARGETS CMake option and the
# generated runtime topology filter in generated/rocm_gpu_targets.h.
ROCM_GPU_TARGETS = [
    "gfx1100",
    "gfx1101",
    "gfx1102",
]

BackendConfigurationInfo = provider(
    fields = {
        "backend": "Resolved backend family.",
        "dynamic": "Whether runtime GPU backend loading is enabled.",
        "linux_cpu": "Whether this is a Linux CPU configuration.",
        "auto_fetch": "Whether runtime backend downloads are enabled.",
    },
)

def _backend_configuration_probe_impl(ctx):
    return [BackendConfigurationInfo(
        backend = ctx.attr.backend,
        dynamic = ctx.attr.dynamic,
        linux_cpu = ctx.attr.linux_cpu,
        auto_fetch = ctx.attr.auto_fetch,
    )]

backend_configuration_probe = rule(
    implementation = _backend_configuration_probe_impl,
    attrs = {
        "backend": attr.string(mandatory = True),
        "dynamic": attr.bool(mandatory = True),
        "linux_cpu": attr.bool(mandatory = True),
        "auto_fetch": attr.bool(mandatory = True),
    },
)

def _backend_configuration_transition_impl(_settings, attr):
    return {
        str(Label("//:backend")): attr.backend,
        str(Label("//:auto_fetch_backends")): attr.auto_fetch,
        "//command_line_option:platforms": [attr.platform],
    }

_backend_configuration_transition = transition(
    implementation = _backend_configuration_transition_impl,
    inputs = [],
    outputs = [
        str(Label("//:backend")),
        str(Label("//:auto_fetch_backends")),
        "//command_line_option:platforms",
    ],
)

def _backend_configuration_case_impl(ctx):
    configuration = ctx.attr.probe[0][BackendConfigurationInfo]
    expected = BackendConfigurationInfo(
        backend = ctx.attr.expected_backend,
        dynamic = ctx.attr.expected_dynamic,
        linux_cpu = ctx.attr.expected_linux_cpu,
        auto_fetch = ctx.attr.expected_auto_fetch,
    )
    for field in ["backend", "dynamic", "linux_cpu", "auto_fetch"]:
        actual_value = getattr(configuration, field)
        expected_value = getattr(expected, field)
        if actual_value != expected_value:
            fail("%s: expected %s=%s, got %s" % (
                ctx.label,
                field,
                expected_value,
                actual_value,
            ))
    return [DefaultInfo()]

backend_configuration_case = rule(
    implementation = _backend_configuration_case_impl,
    attrs = {
        "backend": attr.string(mandatory = True),
        "auto_fetch": attr.bool(default = False),
        "expected_backend": attr.string(mandatory = True),
        "expected_dynamic": attr.bool(default = False),
        "expected_auto_fetch": attr.bool(default = False),
        "expected_linux_cpu": attr.bool(default = False),
        "platform": attr.label(mandatory = True),
        "probe": attr.label(
            cfg = _backend_configuration_transition,
            mandatory = True,
            providers = [BackendConfigurationInfo],
        ),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)

def _rocm_targets_header_impl(ctx):
    targets = ", ".join(['"%s"' % target for target in ctx.attr.targets])
    ctx.actions.write(
        output = ctx.outputs.out,
        content = """#ifndef LLM_CC_GENERATED_ROCM_GPU_TARGETS_H_
#define LLM_CC_GENERATED_ROCM_GPU_TARGETS_H_

#include <array>
#include <string_view>

namespace llmcc {
inline constexpr auto kRocmGpuTargets =
    std::to_array<std::string_view>({%s});
}  // namespace llmcc

#endif  // LLM_CC_GENERATED_ROCM_GPU_TARGETS_H_
""" % targets,
    )

rocm_targets_header = rule(
    implementation = _rocm_targets_header_impl,
    attrs = {
        "out": attr.output(mandatory = True),
        "targets": attr.string_list(mandatory = True),
    },
)

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

def _runfiles_path(ctx, file):
    if file.short_path.startswith("../"):
        return file.short_path[3:]
    return ctx.workspace_name + "/" + file.short_path

def _install_launcher_impl(ctx):
    launcher = ctx.actions.declare_file(ctx.label.name)
    workspace = ctx.workspace_name
    bundle_arguments = ""
    bundle_files = []
    for target in ctx.attr.bundles:
        outputs = target[OutputGroupInfo]
        bundles = outputs.bundle.to_list()
        checksums = outputs.checksum.to_list()
        manifests = outputs.manifest.to_list()
        if len(bundles) != 1 or len(checksums) != 1 or len(manifests) != 1:
            fail("each install backend bundle must provide one bundle, checksum, and manifest")
        for file in bundles + checksums + manifests:
            bundle_files.append(file)
        bundle_arguments += " \\\n  --bundle \"$runfiles_dir/{bundle}\" \\\n  --checksum \"$runfiles_dir/{checksum}\" \\\n  --manifest \"$runfiles_dir/{manifest}\"".format(
            workspace = workspace,
            bundle = _runfiles_path(ctx, bundles[0]),
            checksum = _runfiles_path(ctx, checksums[0]),
            manifest = _runfiles_path(ctx, manifests[0]),
        )
    ctx.actions.write(
        output = launcher,
        content = """#!/usr/bin/env bash
set -euo pipefail
runfiles_dir="${{RUNFILES_DIR:-$0.runfiles}}"
exec "$runfiles_dir/{script}" \\
  "$runfiles_dir/{binary}"{bundle_arguments} \\
  -- \\
  "$@"
""".format(
            workspace = workspace,
            script = _runfiles_path(ctx, ctx.file.script),
            binary = _runfiles_path(ctx, ctx.file.binary),
            bundle_arguments = bundle_arguments,
        ),
        is_executable = True,
    )
    runfiles = ctx.runfiles(files = [
        ctx.file.binary,
        ctx.file.script,
    ] + bundle_files)
    return [DefaultInfo(
        executable = launcher,
        runfiles = runfiles,
    )]

install_launcher = rule(
    implementation = _install_launcher_impl,
    executable = True,
    attrs = {
        "binary": attr.label(allow_single_file = True, mandatory = True),
        "bundles": attr.label_list(),
        "script": attr.label(allow_single_file = True, mandatory = True),
    },
)

def _file_short_path(file):
    return file.short_path

def _rocm_runtime_destination(source):
    root = source.owner.workspace_root + "/"
    if not source.path.startswith(root):
        fail("ROCm runtime input is outside its repository: %s" % source.path)
    return source.path[len(root):]

def _embedded_linux_binary_impl(ctx):
    binary_files = ctx.attr.binary[DefaultInfo].files.to_list()
    cuda_files = ctx.attr.cuda_module[DefaultInfo].files.to_list()
    rocm_module_files = ctx.attr.rocm_module[DefaultInfo].files.to_list()
    if len(binary_files) != 1 or len(cuda_files) != 1 or len(rocm_module_files) != 1:
        fail("embedded distribution inputs must each produce exactly one file")

    output = ctx.actions.declare_file(ctx.attr.output_name)
    checksum = ctx.actions.declare_file(ctx.attr.output_name + ".sha256")
    args = ctx.actions.args()
    args.add("--binary", binary_files[0])
    args.add("--cuda", cuda_files[0])
    args.add("--rocm-module", rocm_module_files[0])
    args.add("--output", output)
    args.add("--checksum-output", checksum)

    runtime_files = ctx.attr.rocm_runtime[DefaultInfo].files.to_list()
    for source in sorted(runtime_files, key = _file_short_path):
        destination = _rocm_runtime_destination(source)
        args.add("--rocm-file", source.path + "=" + destination)

    ctx.actions.run(
        executable = ctx.executable._packager,
        arguments = [args],
        inputs = depset(
            direct = binary_files + cuda_files + rocm_module_files,
            transitive = [ctx.attr.rocm_runtime[DefaultInfo].files],
        ),
        outputs = [output, checksum],
        mnemonic = "EmbedLlmCcPayloads",
        progress_message = "Embedding CUDA and ROCm payloads in %{output}",
    )
    return [
        DefaultInfo(files = depset([output, checksum])),
        OutputGroupInfo(
            checksum = depset([checksum]),
            executable = depset([output]),
        ),
    ]

embedded_linux_binary = rule(
    implementation = _embedded_linux_binary_impl,
    attrs = {
        "binary": attr.label(mandatory = True),
        "cuda_module": attr.label(mandatory = True),
        "output_name": attr.string(mandatory = True),
        "rocm_module": attr.label(mandatory = True),
        "rocm_runtime": attr.label(mandatory = True),
        "_packager": attr.label(
            executable = True,
            cfg = "exec",
            default = Label("//tools:embed_payloads"),
        ),
    },
)

def _backend_bundle_impl(ctx):
    module_files = ctx.attr.module[DefaultInfo].files.to_list()
    if len(module_files) != 1:
        fail("backend bundle module must produce exactly one file")

    runtime_files = []
    if ctx.attr.runtime:
        runtime_files = ctx.attr.runtime[DefaultInfo].files.to_list()
    if ctx.attr.backend != "rocm" and runtime_files:
        fail("backend bundle runtime files are only supported for ROCm")

    output_dir = ctx.label.name
    output = ctx.actions.declare_file(output_dir + "/" + ctx.attr.output_name)
    checksum = ctx.actions.declare_file(output_dir + "/" + ctx.attr.output_name + ".sha256")
    manifest = ctx.actions.declare_file(output_dir + "/manifest.json")

    args = ctx.actions.args()
    args.add(
        "--file",
        module_files[0].path + "=libllm-cc-backend-%s.so" % ctx.attr.backend,
    )
    if ctx.attr.backend == "rocm":
        for source in sorted(runtime_files, key = _file_short_path):
            args.add(
                "--file",
                source.path + "=" + _rocm_runtime_destination(source),
            )

    ctx.actions.run_shell(
        arguments = [
            ctx.attr._provenance[ProvenanceInfo].metadata.path,
            ctx.executable._packager.path,
            output.path,
            checksum.path,
            manifest.path,
            ctx.attr.backend,
            args,
        ],
        command = """
set -euo pipefail
source "$1"
packager="$2"
output="$3"
checksum="$4"
manifest="$5"
backend="$6"
shift 6
exec "$packager" \
  --write-bundle "$output" \
  --name "$backend" \
  --manifest "$manifest" \
  --checksum-output "$checksum" \
  --version "$version" \
  --git-sha "$git_sha" \
  --configuration "$configuration" \
  --llama-commit "" \
  --ggml-abi "" \
  "$@"
""",
        inputs = depset(
            direct = module_files + [ctx.attr._provenance[ProvenanceInfo].metadata],
            transitive = [ctx.attr.runtime[DefaultInfo].files] if ctx.attr.runtime else [],
        ),
        outputs = [output, checksum, manifest],
        tools = [ctx.executable._packager],
        mnemonic = "LlmCcBackendBundle",
        progress_message = "Creating %{label} backend bundle",
    )
    return [
        DefaultInfo(files = depset([output, checksum, manifest])),
        OutputGroupInfo(
            bundle = depset([output]),
            checksum = depset([checksum]),
            manifest = depset([manifest]),
        ),
    ]

backend_bundle = rule(
    implementation = _backend_bundle_impl,
    attrs = {
        "_provenance": attr.label(default = Label("//:version_header"), providers = [ProvenanceInfo]),
        "backend": attr.string(mandatory = True, values = ["cuda", "rocm"]),
        "module": attr.label(mandatory = True),
        "output_name": attr.string(mandatory = True),
        "runtime": attr.label(),
        "_packager": attr.label(
            executable = True,
            cfg = "exec",
            default = Label("//tools:embed_payloads"),
        ),
    },
)

def _payload_archive_impl(ctx):
    output = ctx.actions.declare_file(ctx.attr.output_name)
    binary_files = ctx.attr.binary[DefaultInfo].files.to_list()
    if len(binary_files) != 1:
        fail("payload binary must produce exactly one file")

    mappings = [(binary_files[0], "llm-cc")]

    args = ctx.actions.args()
    args.add("--output", output)
    args.add("--root", ctx.attr.root_name)
    if ctx.attr.require_static:
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
        "output_name": attr.string(mandatory = True),
        "require_static": attr.bool(default = True),
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

def _backend_locations_impl(ctx):
    # Labels carry the owning module's repository mapping, including renamed
    # dependencies. Reading their metadata does not build the GPU target.
    repo = Label("@llama_cpp//:libllm-cc-backend-cuda.so")
    ctx.actions.write(ctx.outputs.out, "\n".join([
        "#ifndef LLM_CC_BACKEND_LOCATIONS_H_",
        "#define LLM_CC_BACKEND_LOCATIONS_H_",
        '#define LLM_CC_BACKEND_EXEC_DIR "%s"' % repo.workspace_root,
        '#define LLM_CC_BACKEND_RUNFILES_DIR "%s"' % repo.repo_name,
        "#endif",
        "",
    ]))

backend_locations = rule(
    implementation = _backend_locations_impl,
    attrs = {"out": attr.output(mandatory = True)},
)

def rocm_runfiles_linkopts():
    root = Label("@rocm_sdk//:sdk").repo_name
    return ["-Wl,-rpath,$$ORIGIN/llm-cc.runfiles/%s/%s" % (root, subdir) for subdir in ["lib", "lib/llvm/lib", "lib/rocm_sysdeps/lib"]]
