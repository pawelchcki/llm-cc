#!/usr/bin/env python3
"""Install and test the generated prebuilt formula through a temporary tap."""

import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading
import uuid


def smoke(packages, version):
    with tempfile.TemporaryDirectory(prefix="llm-cc-brew-") as temporary:
        root = Path(temporary)
        env = dict(os.environ, HOMEBREW_NO_AUTO_UPDATE="1",
                   HOMEBREW_NO_INSTALL_CLEANUP="1", HOMEBREW_CACHE=str(root / "cache"),
                   HOMEBREW_LOGS=str(root / "logs"), HOMEBREW_NO_ANALYTICS="1")
        handler = partial(SimpleHTTPRequestHandler, directory=str(packages))
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        tap = f"llm-cc-test/smoke-{uuid.uuid4().hex[:12]}"
        full_name = f"{tap}/llm-cc"

        def brew(*arguments, check=True):
            return subprocess.run(["brew", *arguments], env=env, check=check, timeout=300,
                                  text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

        try:
            if brew("list", "--formula", "--versions", "llm-cc", check=False).returncode == 0:
                raise RuntimeError("Homebrew smoke test needs llm-cc to be absent from the Homebrew prefix")
            source = root / "tap"
            (source / "Formula").mkdir(parents=True)
            # Change only the transport; retain the published hashes and DSL.
            formula = (packages / "llm-cc.rb").read_text()
            formula = re.sub(r"https://github.com/pawelchcki/llm-cc/releases/download/v[0-9.]+/",
                             f"http://127.0.0.1:{server.server_port}/", formula)
            (source / "Formula/llm-cc.rb").write_text(formula)
            subprocess.run(["git", "init", str(source)], check=True, capture_output=True)
            subprocess.run(["git", "-C", str(source), "add", "Formula"], check=True)
            subprocess.run(["git", "-C", str(source), "-c", "user.name=Distribution test",
                            "-c", "user.email=test@example.invalid", "commit", "-m", "Test formula"],
                           check=True, capture_output=True)
            print(brew("tap", "--custom-remote", tap, str(source)).stdout)
            print(brew("install", "--formula", full_name).stdout)
            print(brew("test", full_name).stdout)
            prefix = Path(brew("--prefix", full_name).stdout.strip())
            actual = subprocess.check_output([str(prefix / "bin/llm-cc"), "--version"], text=True)
            if actual.strip() != f"llm-cc {version}":
                raise AssertionError(actual)
        except subprocess.CalledProcessError as error:
            print(error.stdout)
            raise
        finally:
            if (root / "tap").exists():
                brew("uninstall", "--force", full_name, check=False)
                brew("untap", tap, check=False)
            server.shutdown()
            server.server_close()
            thread.join()
    print("Homebrew prebuilt install and formula test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packages", required=True, type=Path)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    smoke(args.packages.resolve(), args.version)


if __name__ == "__main__":
    main()
