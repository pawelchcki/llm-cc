"""Small build helpers for configuring the vendored inference backend."""

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
