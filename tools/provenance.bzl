"""One source of llm-cc provenance for the executable and backend bundles."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

ProvenanceInfo = provider(fields = ["metadata", "header"])

# Pinned inputs and build recipes are hashed by content, never by their external
# repository name. Moving the same source into a consumer keeps the same key.
def _provenance_impl(ctx):
    header = ctx.actions.declare_file("generated/version.h")
    metadata = ctx.actions.declare_file("generated/provenance.env")
    args = ctx.actions.args()
    args.add(header)
    args.add(metadata)
    args.add(ctx.file.version)
    args.add(ctx.attr.source_version[BuildSettingInfo].value)
    args.add(ctx.attr.source_commit[BuildSettingInfo].value)
    args.add(ctx.attr.artifact_base_url[BuildSettingInfo].value)
    args.add("|".join([ctx.attr.archs[BuildSettingInfo].value, ctx.var.get("TARGET_CPU", ""), ctx.var.get("COMPILATION_MODE", "")]))
    inputs = [ctx.file.version] + ctx.files.configuration
    # An external module must never inherit the consumer's workspace status.
    if not ctx.label.workspace_root and ctx.attr.stamp:
        args.add(ctx.info_file)
        inputs.append(ctx.info_file)
    else:
        args.add("")
    args.add_all(ctx.files.configuration)
    ctx.actions.run(
        executable = ctx.executable._generator,
        arguments = [args],
        inputs = inputs,
        outputs = [header, metadata],
        mnemonic = "LlmCcProvenance",
    )
    return [DefaultInfo(files = depset([header])), ProvenanceInfo(metadata = metadata, header = header)]

provenance = rule(
    implementation = _provenance_impl,
    attrs = {
        "version": attr.label(default = Label("//:version.txt"), allow_single_file = True),
        "source_version": attr.label(default = Label("//:source_version")),
        "source_commit": attr.label(default = Label("//:source_commit")),
        "artifact_base_url": attr.label(default = Label("//:artifact_base_url")),
        "archs": attr.label(default = Label("//:cuda_archs")),
        "stamp": attr.bool(),
        "configuration": attr.label_list(allow_files = True, default = [
            Label("//:extensions.bzl"),
            Label("//:MODULE.bazel"),
            Label("//tools:gpu_sdk_repositories.bzl"),
            Label("//tools:build_configuration.bzl"),
            Label("//tools:cuda_host_compiler_wrapper.sh"),
            Label("//tools:cuda_glibc_compat.h"),
            Label("//tools:hip_library.bzl"),
            Label("//third_party:llama_cpp.BUILD.bazel"),
            Label("//third_party:llama_cpp_backend_score.patch"),
            Label("//third_party:llama_cpp_deterministic_hip_cuid.patch"),
            Label("//third_party:llama_cpp_namespaced_backends.patch"),
            Label("//third_party:backend_module.lds"),
            Label("//third_party:ggml_backend_abi.list"),
            Label("//third_party:cuda_host_toolchain.BUILD.bazel"),
            Label("//third_party:rules_cuda_explicit_tools.patch"),
            Label("//third_party:rocm_sdk.BUILD.bazel"),
        ]),
        "_generator": attr.label(default = Label("//tools:generate_provenance"), executable = True, cfg = "exec"),
    },
)
