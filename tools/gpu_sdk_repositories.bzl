"""Pinned accelerator SDK repositories used by llama.cpp foreign builds."""

_CUDA_COMPONENTS = [
    struct(
        name = "nvcc",
        sha256 = "48e35be3cfbf4b4fbc16828eaec8a7048ee789403049dc409f7b643d6259cf7a",
        strip_prefix = "cuda_nvcc-linux-x86_64-13.0.88-archive",
        url = "https://developer.download.nvidia.com/compute/cuda/redist/cuda_nvcc/linux-x86_64/cuda_nvcc-linux-x86_64-13.0.88-archive.tar.xz",
    ),
    struct(
        name = "nvprune",
        sha256 = "b182d8b0398ba9ff8ee75fc2ffef3ff1e2069e6bec6938bcf02a6c9e69d40456",
        strip_prefix = "cuda_nvprune-linux-x86_64-13.0.85-archive",
        url = "https://developer.download.nvidia.com/compute/cuda/redist/cuda_nvprune/linux-x86_64/cuda_nvprune-linux-x86_64-13.0.85-archive.tar.xz",
    ),
    struct(
        name = "nvvm",
        sha256 = "17ef1665b63670887eeba7d908da5669fa8c66bb73b5b4c1367f49929c086353",
        strip_prefix = "libnvvm-linux-x86_64-13.0.88-archive",
        url = "https://developer.download.nvidia.com/compute/cuda/redist/libnvvm/linux-x86_64/libnvvm-linux-x86_64-13.0.88-archive.tar.xz",
    ),
    struct(
        name = "crt",
        sha256 = "5a3279a049ffc1cdb951c44cb95206acfdde9e9ae5e87825fc18d7e4a6878bb0",
        strip_prefix = "cuda_crt-linux-x86_64-13.0.88-archive",
        url = "https://developer.download.nvidia.com/compute/cuda/redist/cuda_crt/linux-x86_64/cuda_crt-linux-x86_64-13.0.88-archive.tar.xz",
    ),
    struct(
        name = "cudart",
        sha256 = "25b8071951baba827be1580b841d363464f6ee6c39f48d33a81646f90cc95ed1",
        strip_prefix = "cuda_cudart-linux-x86_64-13.0.96-archive",
        url = "https://developer.download.nvidia.com/compute/cuda/redist/cuda_cudart/linux-x86_64/cuda_cudart-linux-x86_64-13.0.96-archive.tar.xz",
    ),
    struct(
        name = "cccl",
        sha256 = "ed845eae8c1767706b6ee91e40c608a03f6f633551a849b63f7346d32d73ee60",
        strip_prefix = "cuda_cccl-linux-x86_64-13.0.85-archive",
        url = "https://developer.download.nvidia.com/compute/cuda/redist/cuda_cccl/linux-x86_64/cuda_cccl-linux-x86_64-13.0.85-archive.tar.xz",
    ),
    struct(
        name = "cublas",
        sha256 = "88bc951efd906032a371153ca61975e0d9c4761e4012169169a6b3a47931606e",
        strip_prefix = "libcublas-linux-x86_64-13.1.0.3-archive",
        url = "https://developer.download.nvidia.com/compute/cuda/redist/libcublas/linux-x86_64/libcublas-linux-x86_64-13.1.0.3-archive.tar.xz",
    ),
]

_CUDA_MERGE_DIRECTORIES = [
    "bin",
    "include",
    "lib",
    "lib64",
    "nvvm",
]

def _overlay_component(repository_ctx, source):
    """Creates a deterministic first-wins overlay of CUDA's stable layout."""
    if not repository_ctx.path("sdk").exists:
        repository_ctx.file("sdk/.llm_cc_overlay", "")
    for child in source.readdir():
        target = "sdk/" + child.basename
        if child.is_dir and child.basename in _CUDA_MERGE_DIRECTORIES:
            if not repository_ctx.path(target).exists:
                repository_ctx.file(target + "/.llm_cc_overlay", "")
            for entry in child.readdir():
                entry_target = target + "/" + entry.basename
                if not repository_ctx.path(entry_target).exists:
                    repository_ctx.symlink(entry, entry_target)
        elif not repository_ctx.path(target).exists:
            repository_ctx.symlink(child, target)

def _cuda_sdk_repository_impl(repository_ctx):
    for component in _CUDA_COMPONENTS:
        output = "components/" + component.name
        repository_ctx.download_and_extract(
            url = component.url,
            output = output,
            sha256 = component.sha256,
            stripPrefix = component.strip_prefix,
        )
        _overlay_component(repository_ctx, repository_ctx.path(output))

    # NVCC's Linux driver searches TOP/lib64 even though current redist
    # components place cudart and cublas in TOP/lib.
    if not repository_ctx.path("sdk/lib64").exists:
        repository_ctx.symlink(repository_ctx.path("sdk/lib"), "sdk/lib64")

    repository_ctx.file(
        "BUILD.bazel",
        """load("@rules_cc//cc:cc_import.bzl", "cc_import")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "sdk",
    srcs = glob(["sdk/**"]),
)

filegroup(
    name = "nvcc",
    srcs = ["sdk/bin/nvcc"],
)

filegroup(
    name = "nvprune",
    srcs = ["sdk/bin/nvprune"],
)

cc_import(
    name = "cudart",
    shared_library = "sdk/lib/libcudart.so.13",
)

cc_import(
    name = "cublas",
    shared_library = "sdk/lib/libcublas.so.13",
)

cc_import(
    name = "cublas_lt",
    shared_library = "sdk/lib/libcublasLt.so.13",
)

filegroup(
    name = "runtime_files",
    # Source-file labels preserve the SDK-relative paths in runfiles.
    srcs = glob([
        "sdk/lib/libcublas.so*",
        "sdk/lib/libcublasLt.so*",
        "sdk/lib/libcudart.so*",
    ]),
)

cc_library(
    name = "runtime",
    # Vendor libraries are runtime data, not direct link dependencies of
    # llm-cc. libggml-cuda records the subset it needs in DT_NEEDED.
    data = [":runtime_files"],
)
""",
    )

cuda_sdk_repository = repository_rule(
    implementation = _cuda_sdk_repository_impl,
    doc = "Downloads and overlays the checksum-pinned CUDA 13.0.2 SDK subset.",
)
