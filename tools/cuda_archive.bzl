"""Hermetic architecture pruning for CUDA static libraries."""

def _cuda_pruned_archive_impl(ctx):
    output = ctx.actions.declare_file(ctx.label.name + ".a")
    args = ctx.actions.args()
    args.add("--generate-code")
    args.add("arch=compute_75,code=[%s]" % ",".join(ctx.attr.archs))
    args.add("--output-file")
    args.add(output)
    args.add(ctx.file.src)
    ctx.actions.run(
        executable = ctx.executable.nvprune,
        arguments = [args],
        inputs = [ctx.file.src],
        outputs = [output],
        mnemonic = "CudaArchivePrune",
        progress_message = "Pruning CUDA archive %{input}",
    )
    return [DefaultInfo(files = depset([output]))]

cuda_pruned_archive = rule(
    implementation = _cuda_pruned_archive_impl,
    attrs = {
        "archs": attr.string_list(mandatory = True),
        "nvprune": attr.label(
            executable = True,
            cfg = "exec",
            default = Label("@cuda_sdk//:nvprune"),
        ),
        "src": attr.label(allow_single_file = [".a"], mandatory = True),
    },
)
