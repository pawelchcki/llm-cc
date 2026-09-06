#!/usr/bin/env python3
"""Exercise the packaged CLI's real HTTP downloader and verified offline cache."""

import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os
import subprocess
import tempfile
import threading


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assets", type=Path)
    parser.add_argument("version")
    args = parser.parse_args()
    assets = args.assets.resolve()
    binary = assets / f"llm-cc-{args.version}-linux-x86_64"
    binary.chmod(0o755)  # Actions artifact downloads do not preserve mode bits.
    server = ThreadingHTTPServer(
        ("127.0.0.1", 0), partial(SimpleHTTPRequestHandler, directory=str(assets))
    )
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="llm-cc-release-smoke-") as cache:
            env = dict(os.environ, LLM_CC_RUNTIME_DIR=cache)
            for backend in ("cuda", "rocm"):
                url = (f"http://127.0.0.1:{server.server_port}/"
                       f"llm-cc-backend-{backend}-linux-x86_64.bundle")
                subprocess.run([str(binary), "backends", "fetch", backend,
                                "--url", url, "--assume-yes"], env=env, check=True)
            server.shutdown()
            thread.join()
            for backend in ("cuda", "rocm"):
                subprocess.run([str(binary), "backends", "fetch", backend,
                                "--no-download"], env=env, check=True)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
