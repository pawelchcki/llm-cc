#!/usr/bin/env python3
"""Install release packages using real npm/npx and pip, with a local registry."""

import argparse
import base64
from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
from urllib.parse import unquote, urlsplit
import venv

from distribution_packages import npm_archive, npm_name
from install_release import target_platform


@contextmanager
def registry(packages):
    """Serve the actual tarballs unchanged, including their npm integrity hashes."""
    metadata = {}
    downloads = []
    for path in packages.glob("*.tgz"):
        with tarfile.open(path) as archive:
            package = json.load(archive.extractfile("package/package.json"))
        metadata[package["name"]] = (package, path)

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            name = unquote(urlsplit(self.path).path).lstrip("/")
            if name.startswith("tarballs/"):
                filename = name.removeprefix("tarballs/")
                paths = {path.name: path for _, path in metadata.values()}
                if filename not in paths:
                    self.send_error(404)
                    return
                downloads.append(filename)
                data = paths[filename].read_bytes()
                content_type = "application/octet-stream"
            elif name in metadata:
                package, path = metadata[name]
                package = dict(package, dist={
                    "tarball": f"http://127.0.0.1:{self.server.server_port}/tarballs/{path.name}",
                    "integrity": "sha512-" + base64.b64encode(hashlib.sha512(path.read_bytes()).digest()).decode(),
                })
                data = json.dumps({
                    "name": name, "dist-tags": {"latest": package["version"]},
                    "versions": {package["version"]: package},
                }).encode()
                content_type = "application/json"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}", downloads
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def run(argv, **kwargs):
    # Resolve npm.cmd/npx.cmd on Windows as well as Unix executables.
    argv = [shutil.which(str(argv[0])) or str(argv[0]), *map(str, argv[1:])]
    return subprocess.run(argv, text=True, capture_output=True, timeout=120, **kwargs)


def require_success(result):
    if result.returncode:
        raise AssertionError(f"{result.args}: {result.returncode}\n{result.stdout}\n{result.stderr}")


def compare(command, reference, arguments, cwd, env, stdin=None):
    expected = run([reference, *arguments], cwd=cwd, env=env, input=stdin)
    actual = run([*command, *arguments], cwd=cwd, env=env, input=stdin)
    if (actual.returncode, actual.stdout, actual.stderr) != (expected.returncode, expected.stdout, expected.stderr):
        raise AssertionError(f"Wrapper differs from binary for {arguments}:\n{expected}\n{actual}")


def smoke(packages, version, reference, fixture=False):
    platform = target_platform()
    suffix = ".exe" if os.name == "nt" else ""
    with tempfile.TemporaryDirectory(prefix="llm-cc-distribution-") as temporary:
        root = Path(temporary)
        # Paths with spaces catch shell splitting and quoting errors.
        cwd = root / "working directory"
        cwd.mkdir()
        env = dict(os.environ, LLM_CC_WRAPPER_TEST="inherited", npm_config_audit="false",
                   npm_config_fund="false", npm_config_update_notifier="false",
                   npm_config_registry="", npm_config_userconfig=str(root / "npmrc"))
        (root / "npmrc").write_text("")
        environment = root / "python environment"
        venv.EnvBuilder(with_pip=True).create(environment)
        scripts = environment / ("Scripts" if os.name == "nt" else "bin")
        python = scripts / ("python.exe" if os.name == "nt" else "python")
        require_success(run([python, "-m", "pip", "install", "--no-index", "--no-deps",
                             "--find-links", packages, f"llm-cc=={version}"]))
        pip_cli = scripts / f"llm-cc{suffix}"
        probes = (["--version"], ["--help"], ["--llm-cc-invalid-wrapper-test-option"])
        if fixture:
            probes += (["--wrapper-probe", "argument with spaces", "", "$(echo literal)", "--flag=value"], ["--terminate"])
        for arguments in probes:
            compare([pip_cli], reference, arguments, cwd, env, stdin="input from parent\n")
            compare([python, "-m", "llm_cc"], reference, arguments, cwd, env, stdin="input from parent\n")

        with registry(packages) as (url, downloads):
            env["npm_config_registry"] = url
            require_success(run(["npm", "install", "--global", "--prefix", root / "npm prefix",
                                 "--cache", root / "npm cache", "--ignore-scripts", f"llm-cc@{version}"],
                                cwd=cwd, env=env))
            prefix = root / "npm prefix"
            node_cli = prefix / ("node_modules" if os.name == "nt" else "lib/node_modules") / "llm-cc/cli.cjs"
            for arguments in probes:
                compare(["node", node_cli], reference, arguments, cwd, env, stdin="input from parent\n")
            # Exercise npm's generated PATH entry as installed for users.
            npm_cli = prefix / ("llm-cc.cmd" if os.name == "nt" else "bin/llm-cc")
            require_success(run([npm_cli, "--version"], cwd=cwd, env=env))
            npx = run(["npx", "--yes", "--cache", root / "npx cache", f"llm-cc@{version}", "--version"],
                      cwd=cwd, env=env)
            require_success(npx)
            if npx.stdout.strip() != f"llm-cc {version}":
                raise AssertionError(npx)
            expected_downloads = {npm_archive("llm-cc", version), npm_archive(npm_name(platform), version)}
            if set(downloads) != expected_downloads:
                raise AssertionError(f"npm fetched unexpected platforms: {downloads}")
            # Missing optional packages must report an actionable error.
            binary_package = node_cli.parent / "node_modules" / npm_name(platform)
            if not binary_package.exists():
                binary_package = node_cli.parent.parent / npm_name(platform)
            shutil.rmtree(binary_package)
            missing = run(["node", node_cli, "--version"], cwd=cwd, env=env)
            if missing.returncode != 1 or "optional dependencies enabled" not in missing.stderr:
                raise AssertionError(missing)
    print(f"pip, python -m, npm install and npx wrappers passed on {platform}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packages", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--binary", required=True, type=Path)
    args = parser.parse_args()
    args.binary.chmod(0o755)
    smoke(args.packages.resolve(), args.version, args.binary.resolve())


if __name__ == "__main__":
    main()
