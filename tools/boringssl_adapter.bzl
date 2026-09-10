"""Expose native BoringSSL artifacts as a CMake dependency install tree."""

load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("@rules_foreign_cc//foreign_cc:providers.bzl", "ForeignCcArtifactInfo", "ForeignCcDepsInfo")

def _archive(target, exclude = []):
    archives = []
    for linker_input in target[CcInfo].linking_context.linker_inputs.to_list():
        for library in linker_input.libraries:
            archive = library.pic_static_library or library.static_library
            if archive and archive not in exclude and archive not in archives:
                archives.append(archive)
    if len(archives) != 1:
        fail("expected one native static archive from %s, got %s" % (target.label, archives))
    return archives[0]

def _boringssl_adapter_impl(ctx):
    tree = ctx.actions.declare_directory(ctx.label.name)
    args = ctx.actions.args()
    args.add(tree.path)
    inputs = []
    crypto = _archive(ctx.attr.crypto)
    ssl = _archive(ctx.attr.ssl, exclude = [crypto])
    for name, archive in [("crypto", crypto), ("ssl", ssl)]:
        inputs.append(archive)
        args.add(archive.path)
        args.add("lib/lib%s.a" % name)
    headers = ctx.attr.ssl[CcInfo].compilation_context.headers
    for header in headers.to_list():
        marker = "/include/openssl/"
        if marker in header.path:
            args.add(header.path)
            args.add("include/openssl/" + header.path.split(marker)[1])
    ctx.actions.run_shell(
        arguments = [args],
        inputs = depset(inputs, transitive = [headers]),
        outputs = [tree],
        command = """set -eu
out="$1"; shift
mkdir -p "$out/lib" "$out/include/openssl"
while [ "$#" -gt 0 ]; do
  mkdir -p "$out/$(dirname "$2")"
  cp -L "$1" "$out/$2"
  shift 2
done
""",
        mnemonic = "BoringSslCmakeTree",
    )
    return [
        DefaultInfo(files = depset([tree])),
        cc_common.merge_cc_infos(cc_infos = [ctx.attr.ssl[CcInfo], ctx.attr.crypto[CcInfo]]),
        ForeignCcDepsInfo(artifacts = depset([ForeignCcArtifactInfo(
            gen_dir = tree, bin_dir_name = "bin", dll_dir_name = "lib",
            include_dir_name = "include", lib_dir_name = "lib",
        )])),
    ]

boringssl_adapter = rule(
    implementation = _boringssl_adapter_impl,
    attrs = {
        "ssl": attr.label(default = Label("@boringssl//:ssl"), providers = [CcInfo]),
        "crypto": attr.label(default = Label("@boringssl//:crypto"), providers = [CcInfo]),
    },
)
