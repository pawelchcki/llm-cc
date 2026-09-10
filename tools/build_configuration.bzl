"""Configuration settings and declared CUDA host compiler inputs."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("@rules_cuda//cuda/private:cuda_helper.bzl", "cuda_helper")
load("@rules_cuda//cuda/private:providers.bzl", "CudaArchsInfo")

DEFAULT_CUDA_ARCHS = "compute_75;compute_80;compute_86:sm_86;compute_89:sm_89;compute_90;compute_120a:sm_120a"

def _cuda_archs_impl(ctx):
    value = ctx.build_setting_value
    if not value.strip() or any([not part.strip() for part in value.split(";")]):
        fail("cuda_archs must contain nonempty rules_cuda architecture specifications")
    value = ";".join([":".join([",".join([code.strip() for code in stage.split(",")]) for stage in spec.split(":")]) for spec in value.split(";")])
    specs = cuda_helper.get_arch_specs(value)
    normalized = []
    for spec in specs:
        codes = sorted({("compute_" if code.virtual else "sm_" if code.gpu else "lto_") + code.arch: True for code in spec.stage2_archs})
        normalized.append("compute_" + spec.stage1_arch + ":" + ",".join(codes))
    value = ";".join(sorted({spec: True for spec in normalized}))
    return [BuildSettingInfo(value = value), CudaArchsInfo(arch_specs = cuda_helper.get_arch_specs(value))]

cuda_archs_flag = rule(implementation = _cuda_archs_impl, build_setting = config.string(flag = True))

def _cuda_host_compiler_impl(ctx):
    wrapper = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.expand_template(
        template = ctx.file.template,
        output = wrapper,
        substitutions = {
            "@CLANG@": ctx.file.compiler.path,
            "@SYSROOT@": ctx.file.sysroot_anchor.dirname.removesuffix("/usr/include"),
            "@GCC_ROOT@": ctx.file.cxx_anchor.path.removesuffix("/x86_64-buildroot-linux-gnu/include/c++/12.3.0/vector"),
            "@COMPAT@": ctx.file.compat.path,
        },
        is_executable = True,
    )
    return [DefaultInfo(
        executable = wrapper,
        files = depset([wrapper, ctx.file.compiler, ctx.file.compat], transitive = [dep[DefaultInfo].files for dep in ctx.attr.inputs]),
    )]

cuda_host_compiler = rule(
    implementation = _cuda_host_compiler_impl,
    executable = True,
    attrs = {
        "template": attr.label(default = Label("//tools:cuda_host_compiler_wrapper.sh"), allow_single_file = True),
        "compiler": attr.label(default = Label("@llvm_toolchain_llvm//:bin/clang++"), allow_single_file = True),
        "sysroot_anchor": attr.label(default = Label("@linux_glibc_sysroot//:usr/include/stdlib.h"), allow_single_file = True),
        "cxx_anchor": attr.label(default = Label("@cuda_host_toolchain//:x86_64-buildroot-linux-gnu/include/c++/12.3.0/vector"), allow_single_file = True),
        "compat": attr.label(default = Label("//tools:cuda_glibc_compat.h"), allow_single_file = True),
        "inputs": attr.label_list(default = [Label("@llvm_toolchain_llvm//:clang"), Label("@linux_glibc_sysroot//:sysroot"), Label("@cuda_host_toolchain//:clang_cuda_host_files"), Label("@cuda_host_toolchain//:libstdcxx_static")]),
    },
)
