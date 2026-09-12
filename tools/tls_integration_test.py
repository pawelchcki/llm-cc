"""Exercise the actual libcurl/TLS download path without external networking."""
import http.server
import os
from pathlib import Path
import ssl
import subprocess
import sys
import threading

PAYLOAD = b"resumable TLS fixture\n" * 1024


def executable_runfile(value):
    if os.name == "nt":
        # Native Windows programs cannot traverse every executable junction
        # created by Bazel's Bash runfiles tree. Use its manifest's real path.
        manifest = Path(os.environ.get("RUNFILES_MANIFEST_FILE") or
                        str(Path(os.environ["TEST_SRCDIR"]) / "MANIFEST"))
        key = value.replace("\\", "/")
        for line in manifest.read_text().splitlines():
            escaped = line.startswith(" ")
            logical, physical = line.lstrip(" ").split(" ", 1)
            if escaped:
                def unescape(text):
                    return text.replace("\\s", " ").replace("\\n", "\n").replace("\\b", "\\")
                logical, physical = unescape(logical), unescape(physical)
            if logical == key:
                return Path(physical)
        raise RuntimeError(f"Executable {key} missing from {manifest}")
    return (Path(os.environ["TEST_SRCDIR"]) / value).resolve(strict=True)


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/ca.crl":
            payload = self.server.crl
            self.send_response(200)
            self.send_header("Content-Type", "application/pkix-crl")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return
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
    probe = executable_runfile(sys.argv[1])
    fixtures = Path(sys.argv[2]).resolve(strict=True).parent
    assert probe.is_file(), probe
    root = Path(os.environ["TEST_TMPDIR"])
    # A short-lived leaf with serverAuth works with Apple SecTrust as well as
    # BoringSSL and Schannel. Generate it at test time so fixtures do not expire.
    # Schannel checks revocation by default. Serve a signed, empty CRL locally
    # so the fixture preserves production certificate-verification settings.
    crl_server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    database = root / "ca.index"
    database.write_text("")
    ca_config = root / "ca.conf"
    ca_config.write_text(f"""[ca]
default_ca = fixture
[fixture]
database = {database.as_posix()}
certificate = {(fixtures / 'ca.pem').as_posix()}
private_key = {(fixtures / 'ca.key').as_posix()}
default_md = sha256
default_crl_days = 1
crl_extensions = crl
[crl]
authorityKeyIdentifier = keyid:always
""")
    subprocess.run(["openssl", "ca", "-gencrl", "-config", str(ca_config),
                    "-out", str(root / "ca.crl.pem")], check=True, capture_output=True)
    subprocess.run(["openssl", "crl", "-in", str(root / "ca.crl.pem"),
                    "-outform", "DER", "-out", str(root / "ca.crl")], check=True, capture_output=True)
    crl_server.crl = (root / "ca.crl").read_bytes()
    extensions = root / "server.ext"
    extensions.write_text("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost\n"
                          f"crlDistributionPoints=URI:http://127.0.0.1:{crl_server.server_port}/ca.crl\n")
    subprocess.run(["openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", str(root / "server.key"), "-out", str(root / "server.csr"),
                    "-subj", "/CN=localhost"], check=True, capture_output=True)
    subprocess.run(["openssl", "x509", "-req", "-in", str(root / "server.csr"),
                    "-CA", str(fixtures / "ca.pem"), "-CAkey", str(fixtures / "ca.key"),
                    "-set_serial", "1", "-days", "7", "-extfile", str(extensions),
                    "-out", str(root / "server.pem")], check=True, capture_output=True)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(root / "server.pem", root / "server.key")
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    crl_thread = threading.Thread(target=crl_server.serve_forever, daemon=True)
    crl_thread.start()
    try:
        for name, host, endpoint, ca, success in [
            ("trusted", "localhost", "data", "ca.pem", True),
            ("untrusted", "localhost", "data", "untrusted.pem", False),
            ("hostname", "127.0.0.1", "data", "ca.pem", False),
            ("redirect", "localhost", "redirect", "ca.pem", True),
            ("resumed", "localhost", "data", "ca.pem", True),
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
        crl_server.shutdown()
        crl_server.server_close()
        crl_thread.join()


if __name__ == "__main__":
    main()
