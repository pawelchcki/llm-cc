"""An S3 GET/PUT server that verifies AWS Signature Version 4 on its own.

`llm-cc compare store` signs requests through libcurl. This server
recomputes each signature independently, keeps objects in memory, and can
fail a key's first requests, so the integration test covers signing,
addressing, absent objects, authorization failures and retries without
network access.

    python3 s3_fake_server.py EXECUTABLE   # run the integration scenario
"""

import hashlib
import hmac
import http.server
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import urllib.parse

ACCESS_KEY = "AKIDEXAMPLE"
SECRET_KEY = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"
SESSION_TOKEN = "session/token+value="
REGION = "eu-west-1"
BUCKET = "bucket"


def _sha256(data):
    return hashlib.sha256(data).hexdigest()


def _hmac(key, message):
    return hmac.new(key, message.encode("utf-8"), hashlib.sha256).digest()


def expected_signature(method, path, headers, signed, payload_hash, secret):
    """SigV4 for S3: the path is used exactly as sent, without re-encoding."""
    canonical_headers = "".join(
        "%s:%s\n" % (name, " ".join(headers[name].split())) for name in signed
    )
    canonical_request = "\n".join(
        [method, path, "", canonical_headers, ";".join(signed), payload_hash]
    )
    amz_date = headers["x-amz-date"]
    date = amz_date[:8]
    scope = "%s/%s/s3/aws4_request" % (date, REGION)
    string_to_sign = "\n".join(
        ["AWS4-HMAC-SHA256", amz_date, scope, _sha256(canonical_request.encode())]
    )
    key = _hmac(("AWS4" + secret).encode("utf-8"), date)
    for part in (REGION, "s3", "aws4_request"):
        key = _hmac(key, part)
    return hmac.new(key, string_to_sign.encode("utf-8"), hashlib.sha256).hexdigest()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _reply(self, status, body=b"", content_type="application/xml"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _error(self, status, code):
        self._reply(status, ("<Error><Code>%s</Code></Error>" % code).encode())

    def _authorized(self, body):
        headers = {name.lower(): value for name, value in self.headers.items()}
        authorization = headers.get("authorization", "")
        prefix = "AWS4-HMAC-SHA256 "
        if not authorization.startswith(prefix):
            return "MissingSignature"
        fields = dict(
            part.strip().split("=", 1)
            for part in authorization[len(prefix):].split(",")
        )
        key, date, region, service, terminal = fields["Credential"].split("/")
        signed = fields["SignedHeaders"].split(";")
        if key != ACCESS_KEY:
            return "InvalidAccessKeyId"
        if (region, service, terminal) != (REGION, "s3", "aws4_request"):
            return "AuthorizationHeaderMalformed"
        if date != headers.get("x-amz-date", "")[:8]:
            return "AuthorizationHeaderMalformed"
        payload_hash = headers.get("x-amz-content-sha256")
        if payload_hash != _sha256(body):
            return "XAmzContentSHA256Mismatch"
        required = {"host", "x-amz-date", "x-amz-content-sha256"}
        if self.server.session_token is not None:
            if headers.get("x-amz-security-token") != self.server.session_token:
                return "InvalidToken"
            required.add("x-amz-security-token")
        if not required.issubset(signed) or signed != sorted(signed):
            return "SignatureDoesNotMatch"
        expected = expected_signature(
            self.command, self.path, headers, signed, payload_hash, SECRET_KEY
        )
        if not hmac.compare_digest(expected, fields["Signature"]):
            return "SignatureDoesNotMatch"
        return None

    def _key(self):
        path = urllib.parse.urlsplit(self.path).path
        bucket, _, key = urllib.parse.unquote(path.lstrip("/")).partition("/")
        return bucket, key

    def _handle(self, body):
        self.server.requests.append((self.command, self.path))
        failure = self._authorized(body)
        if failure is not None:
            return self._error(403, failure)
        bucket, key = self._key()
        if bucket != BUCKET:
            return self._error(404, "NoSuchBucket")
        remaining = self.server.failures.get(key, 0)
        if remaining:
            self.server.failures[key] = remaining - 1
            return self._error(503, "SlowDown")
        if self.command == "PUT":
            self.server.objects[key] = body
            return self._reply(200, content_type="text/plain")
        if key not in self.server.objects:
            return self._error(404, "NoSuchKey")
        self._reply(200, self.server.objects[key], "application/octet-stream")

    def do_GET(self):
        self._handle(b"")

    def do_PUT(self):
        length = int(self.headers.get("Content-Length", "0"))
        self._handle(self.rfile.read(length))

    def log_message(self, *_):
        pass


class FakeS3(http.server.ThreadingHTTPServer):
    def __init__(self, session_token=SESSION_TOKEN):
        super().__init__(("127.0.0.1", 0), Handler)
        self.objects = {}
        self.failures = {}
        self.requests = []
        self.session_token = session_token

    @property
    def endpoint(self):
        return "http://127.0.0.1:%d" % self.server_address[1]


def executable_runfile(value):
    if os.name == "nt":
        manifest = Path(
            os.environ.get("RUNFILES_MANIFEST_FILE")
            or str(Path(os.environ["TEST_SRCDIR"]) / "MANIFEST")
        )
        for line in manifest.read_text().splitlines():
            logical, _, physical = line.partition(" ")
            if logical == value.replace("\\", "/"):
                return Path(physical)
        raise RuntimeError("executable %s missing from %s" % (value, manifest))
    return (Path(os.environ["TEST_SRCDIR"]) / value).resolve(strict=True)


def run_scenario(executable):
    server = FakeS3()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = bytes(range(256)) * 64 + b"\r\nend"
            source = root / "payload.bin"
            source.write_bytes(payload)
            options = root / "options.json"
            options.write_text(
                '{"endpoint_url": "%s", "region_name": "%s"}'
                % (server.endpoint, REGION)
            )
            environment = {
                name: value
                for name, value in os.environ.items()
                if not name.startswith("AWS_")
            }
            environment.update(
                AWS_ACCESS_KEY_ID=ACCESS_KEY,
                AWS_SECRET_ACCESS_KEY=SECRET_KEY,
                AWS_SESSION_TOKEN=SESSION_TOKEN,
            )

            def store(action, key, *extra, env=None):
                return subprocess.run(
                    [str(executable), "compare", "store", action, key,
                     "--cache", "s3://%s/prefix/nested/" % BUCKET,
                     "--store-options", str(options), *extra],
                    env=env or environment,
                    capture_output=True,
                    timeout=60,
                )

            key = "results/v2/a b+c~(1).json"
            written = store("put", key, "--input", str(source))
            assert written.returncode == 0, written.stderr
            assert server.objects["prefix/nested/" + key] == payload
            read = store("get", key)
            assert read.returncode == 0, read.stderr
            assert read.stdout == payload, "object bytes round-trip"
            output = root / "copy.bin"
            copied = store("get", key, "--output", str(output))
            assert copied.returncode == 0 and output.read_bytes() == payload

            missing = store("get", "results/v2/absent.json")
            assert missing.returncode == 1, missing
            assert b"not found" in missing.stderr

            wrong = dict(environment, AWS_SECRET_ACCESS_KEY="not-the-secret")
            denied = store("get", key, env=wrong)
            assert denied.returncode == 2, denied
            assert b"HTTP 403" in denied.stderr, denied.stderr
            assert b"SignatureDoesNotMatch" in denied.stderr, denied.stderr

            server.failures["prefix/nested/flaky"] = 2
            server.objects["prefix/nested/flaky"] = b"eventually"
            flaky = store("get", "flaky")
            assert flaky.returncode == 0 and flaky.stdout == b"eventually", flaky

            server.failures["prefix/nested/broken"] = 3
            broken = store("put", "broken", "--input", str(source))
            assert broken.returncode == 2 and b"after 3 attempts" in broken.stderr

            # The endpoint and region may also come from the environment.
            environment.update(AWS_ENDPOINT_URL_S3=server.endpoint, AWS_REGION=REGION)
            from_environment = subprocess.run(
                [str(executable), "compare", "store", "get", key,
                 "--cache", "s3://%s/prefix/nested" % BUCKET],
                env=environment,
                capture_output=True,
                timeout=60,
            )
            assert from_environment.stdout == payload, from_environment.stderr
    finally:
        server.shutdown()
    print("s3 integration scenario passed (%d requests)" % len(server.requests))


if __name__ == "__main__":
    run_scenario(executable_runfile(sys.argv[1]))
