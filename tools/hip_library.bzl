"""Hermetic, source-granular HIP backend compilation."""

def _hip_library_impl(ctx):
    compiler = ctx.executable.compiler
    if compiler.path.endswith("/bin/hipcc"):
        rocm_root = compiler.path.removesuffix("/bin/hipcc")
    else:
        rocm_root = compiler.path.removesuffix("/lib/llvm/bin/clang++")
    cxx_root = ctx.file.cxx_anchor.path.removesuffix("/include/c++/v1/algorithm")
    sysroot_root = ctx.file.sysroot_anchor.path.removesuffix("/usr/include/stdlib.h")
    source_root = ctx.label.workspace_root
    if ctx.label.package:
        source_root += "/" + ctx.label.package

    materialized_cxx = ctx.actions.declare_directory(ctx.label.name + "_cxx_headers")
    materialize_args = ctx.actions.args()
    materialize_args.add(materialized_cxx.path)
    materialize_args.add(cxx_root + "/include/c++/v1")
    materialize_args.add("include/c++/v1")
    materialize_args.add(cxx_root + "/include/x86_64-unknown-linux-gnu/c++/v1")
    materialize_args.add("include/x86_64-unknown-linux-gnu/c++/v1")
    # HIP names Clang's CUDA wrapper explicitly. A second physical libc++ tree
    # gives that wrapper a valid include_next target without exposing symlink
    # canonicalization outside the action sandbox.
    for wrapper_header in ["algorithm", "complex", "new"]:
        materialize_args.add(cxx_root + "/include/c++/v1/" + wrapper_header)
        materialize_args.add("include_after_wrappers/c++/v1/" + wrapper_header)
    ctx.actions.run(
        executable = ctx.executable.materializer,
        arguments = [materialize_args],
        inputs = ctx.attr.cxx_headers[DefaultInfo].files,
        outputs = [materialized_cxx],
        mnemonic = "MaterializeHipHeaders",
        progress_message = "Materializing hermetic libc++ headers",
    )

    common = [
        "-x", "hip",
        "--rocm-path=" + rocm_root,
        "--rocm-device-lib-path=" + rocm_root + "/lib/llvm/amdgcn/bitcode",
        "-resource-dir", rocm_root + "/lib/llvm/lib/clang/23",
        "-include", rocm_root + "/lib/llvm/lib/clang/23/include/__clang_hip_runtime_wrapper.h",
        "--sysroot=" + sysroot_root,
        "-stdlib=libc++",
        "-nostdinc++",
        "-isystem", materialized_cxx.path + "/include/c++/v1",
        "-isystem", materialized_cxx.path + "/include/x86_64-unknown-linux-gnu/c++/v1",
        "-idirafter", materialized_cxx.path + "/include_after_wrappers/c++/v1",
        "-fPIC",
        "-ffunction-sections",
        "-fdata-sections",
        "-O3",
        "-DNDEBUG",
        "-DGGML_BACKEND_BUILD",
        "-DGGML_BACKEND_DL",
        "-DGGML_BACKEND_SHARED",
        "-DGGML_USE_HIP",
        "-D__HIP_PLATFORM_AMD__=1",
        "-DGGML_CUDA_PEER_MAX_BATCH_SIZE=128",
        "-ffile-prefix-map=.=.",
        "-fdebug-prefix-map=.=.",
    ]
    for arch in ctx.attr.archs:
        common.append("--offload-arch=" + arch)
    for include in ctx.attr.includes:
        common.extend(["-I", source_root + "/" + include])
    common.extend(["-I", rocm_root + "/include"])

    inputs = depset(
        direct = ctx.files.hdrs,
        transitive = [
            ctx.attr.compiler_files[DefaultInfo].files,
            ctx.attr.sysroot_files[DefaultInfo].files,
        ],
    )
    objects = []
    for index, src in enumerate(ctx.files.srcs):
        obj = ctx.actions.declare_file("%s_objs/%d_%s.o" % (
            ctx.label.name,
            index,
            src.basename.removesuffix(".cu"),
        ))
        cuid = "llm_cc_%d" % index
        args = ctx.actions.args()
        args.add(compiler.path)
        args.add_all(common)
        args.add("-cuid=" + cuid)
        args.add("-c")
        args.add(src)
        args.add("-o")
        args.add(obj)
        ctx.actions.run(
            executable = ctx.executable.wrapper,
            arguments = [args],
            inputs = depset([src, materialized_cxx], transitive = [inputs]),
            outputs = [obj],
            mnemonic = "HipCompile",
            progress_message = "Compiling HIP %s" % src.short_path,
        )
        objects.append(obj)

    output = ctx.actions.declare_file(ctx.attr.output_name)
    link_args = ctx.actions.args()
    link_args.add(rocm_root + "/lib/llvm/bin/clang++")
    link_args.add_all([
        "--rocm-path=" + rocm_root,
        "--sysroot=" + sysroot_root,
        "-nostdlib++",
        "--rtlib=compiler-rt",
        "--unwindlib=libunwind",
        "-shared",
        "-Wl,--build-id=none",
        "-Wl,--exclude-libs,ALL",
        "-Wl,--gc-sections",
        "-Wl,--disable-new-dtags",
        "-Wl,-rpath,$ORIGIN/lib",
        "-Wl,-rpath,$ORIGIN/lib/llvm/lib",
        "-Wl,-rpath,$ORIGIN/lib/rocm_sysdeps/lib",
        "-Wl,--version-script=" + ctx.file.version_script.path,
    ])
    link_args.add_all(objects)
    link_args.add_all([
        rocm_root + "/lib/libamdhip64.so.7.14.60850-0000000",
        rocm_root + "/lib/libhipblas.so.3.5",
        rocm_root + "/lib/librocblas.so.5.5",
        "-L" + cxx_root + "/lib/x86_64-unknown-linux-gnu",
        "-l:libc++.a",
        "-l:libc++abi.a",
        "-l:libunwind.a",
        "-ldl",
        "-lpthread",
        "-o",
    ])
    link_args.add(output)
    ctx.actions.run(
        executable = ctx.executable.wrapper,
        arguments = [link_args],
        inputs = depset(
            direct = objects + [ctx.file.version_script],
            transitive = [
                ctx.attr.compiler_files[DefaultInfo].files,
                ctx.attr.cxx_libs[DefaultInfo].files,
                ctx.attr.link_files[DefaultInfo].files,
                ctx.attr.sysroot_files[DefaultInfo].files,
            ],
        ),
        outputs = [output],
        mnemonic = "HipLink",
        progress_message = "Linking HIP backend %{output}",
    )
    return [DefaultInfo(files = depset([output]))]

hip_library = rule(
    implementation = _hip_library_impl,
    attrs = {
        "archs": attr.string_list(mandatory = True),
        "compiler": attr.label(executable = True, cfg = "exec", mandatory = True),
        "compiler_files": attr.label(mandatory = True),
        "cxx_anchor": attr.label(allow_single_file = True, mandatory = True),
        "cxx_headers": attr.label(mandatory = True),
        "cxx_libs": attr.label(mandatory = True),
        "hdrs": attr.label_list(allow_files = True),
        "includes": attr.string_list(),
        "link_files": attr.label(mandatory = True),
        "materializer": attr.label(
            executable = True,
            cfg = "exec",
            default = Label("//tools:materialize_tree"),
        ),
        "output_name": attr.string(mandatory = True),
        "srcs": attr.label_list(allow_files = [".cu"], mandatory = True),
        "sysroot_anchor": attr.label(allow_single_file = True, mandatory = True),
        "sysroot_files": attr.label(mandatory = True),
        "version_script": attr.label(allow_single_file = True, mandatory = True),
        "wrapper": attr.label(
            executable = True,
            cfg = "exec",
            default = Label("//tools:hermetic_clang_wrapper"),
        ),
    },
)
