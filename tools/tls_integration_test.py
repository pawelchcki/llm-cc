"""Exercise the actual libcurl/TLS download path without external networking."""
import http.server
import os
from pathlib import Path
import ssl
import subprocess
import sys
import threading

PAYLOAD = b"resumable TLS fixture\n" * 1024


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/data")
            self.end_headers()
            return
        offset = int(self.headers.get("Range", "bytes=0-").split("=")[1].split("-")[0])
        self.send_response(206 if offset else 200)
        self.send_header("Content-Length", str(len(PAYLOAD) - offset))
        if offset:
            self.send_header("Content-Range", f"bytes {offset}-{len(PAYLOAD)-1}/{len(PAYLOAD)}")
        self.end_headers()
        self.wfile.write(PAYLOAD[offset:])

    def log_message(self, *_):
        pass


def main():
    probe, fixtures = map(Path, sys.argv[1:])
    root = Path(os.environ["TEST_TMPDIR"])
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(fixtures / "server.pem", fixtures / "server.key")
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        for name, host, endpoint, ca, success in [
            ("trusted", "localhost", "data", "server.pem", True),
            ("untrusted", "localhost", "data", "untrusted.pem", False),
            ("hostname", "127.0.0.1", "data", "server.pem", False),
            ("redirect", "localhost", "redirect", "server.pem", True),
            ("resumed", "localhost", "data", "server.pem", True),
        ]:
            target = root / name
            if name == "resumed":
                Path(str(target) + ".partial").write_bytes(PAYLOAD[:17])
            env = dict(os.environ, SSL_CERT_FILE=str(fixtures / ca), NO_PROXY="localhost,127.0.0.1", no_proxy="localhost,127.0.0.1")
            result = subprocess.run([str(probe), f"https://{host}:{server.server_port}/{endpoint}", str(target)], env=env, capture_output=True, text=True)
            assert (result.returncode == 0) == success, (name, result.stderr)
            if success:
                assert target.read_bytes() == PAYLOAD, name
            else:
                assert not target.exists(), name
        print("TLS trust, hostname, redirect, and resume checks passed")
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
