"""Public, pinned build repositories for llm-cc and consuming Bazel modules.

Roots must explicitly register toolchains and apply the overrides in the example.
"""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@toolchains_llvm//toolchain:rules.bzl", "llvm_toolchain")
load("@rules_cuda//cuda/private:redist_json_helper.bzl", "redist_json_helper")
load("@rules_cuda//cuda/private:repositories.bzl", "cuda_component", "cuda_toolkit")
load("//tools:gpu_sdk_repositories.bzl", "cuda_sdk_repository")

def _toolchains_impl(ctx):
    # Pinned GCC bundle used only for its libstdc++ headers and libraries. CUDA's
    # x86 headers reject libc++, but NVCC still delegates every host compile to
    # pinned Clang.
    http_archive(
        name = "cuda_host_toolchain",
        build_file = Label("//third_party:cuda_host_toolchain.BUILD.bazel"),
        patch_strip = 1,
        patches = [Label("//third_party:cuda_host_sysroot_package.patch")],
        sha256 = "d6eca7f1ea736ef6f868a027a9d0baa875f9513755026aed2badc04a2b9cd7bd",
        strip_prefix = "x86-64--glibc--stable-2023.08-1",
        urls = ["https://toolchains.bootlin.com/downloads/releases/toolchains/x86-64/tarballs/x86-64--glibc--stable-2023.08-1.tar.bz2"],
    )

    # Chromium's checksum-addressed Debian Stretch sysroot supplies glibc 2.24,
    # keeping the release ABI below the documented glibc 2.28 ceiling.
    http_archive(
        name = "linux_glibc_sysroot",
        build_file_content = """
package(default_visibility = ["//visibility:public"])
filegroup(name = "sysroot", srcs = glob(["**"]))
exports_files(["usr/include/stdlib.h"])
    """,
        sha256 = "73d828bf653f8f8548be704ce1ded4d195bf2bf937fc5426dd5636a9940d1969",
        urls = ["https://commondatastorage.googleapis.com/chrome-linux-sysroot/toolchain/3c248ba4290a5ad07085b7af07e6785bf1ae5b66/debian_stretch_amd64_sysroot.tar.xz"],
    )
    llvm_versions = {"": "22.1.7", "darwin-x86_64": "20.1.7"}
    llvm_toolchain(name = "llvm_toolchain", llvm_versions = llvm_versions, cxx_standard = {"": "c++20"})
    llvm_toolchain(
        name = "llvm_toolchain_portable",
        llvm_versions = llvm_versions,
        cxx_standard = {"": "c++20"},
        toolchain_roots = {"": str(Label("@llvm_toolchain_llvm//:BUILD")).split(":")[0]},
        sysroot = {"linux-x86_64": str(Label("@linux_glibc_sysroot//:sysroot"))},
    )
    cuda_sdk_repository(name = "cuda_sdk")
    redist = struct(version = "13.0.2", urls = [], integrity = "", sha256 = "", components = [])
    url, manifest = redist_json_helper.get(ctx, redist)
    mapping = {}
    for spec in redist_json_helper.collect_specs(ctx, redist, manifest, url):
        name = redist_json_helper.get_repo_name(ctx, spec)
        cuda_component(name = name, **spec)
        mapping[spec["component_name"]] = "@" + name
    cuda_toolkit(name = "cuda", components_mapping = mapping, version = "13.0.2")

toolchains = module_extension(
    implementation = _toolchains_impl,
    os_dependent = True,
    arch_dependent = True,
)
