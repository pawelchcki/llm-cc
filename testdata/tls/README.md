Local-only TLS fixture. The intentionally public CA key must never be used by a
deployed service. `ca.pem` is a test CA and `untrusted.pem` is an unrelated CA.
The roots expire in September 2036. The test generates a seven-day localhost
leaf with serverAuth using an OpenSSL-compatible CLI (OpenSSL or LibreSSL), then
binds an ephemeral loopback port and verifies trust, hostname, redirects and
range resumption through the application's configured TLS backend.
An empty, signed CRL is served over loopback HTTP for Schannel's default
revocation check; certificate verification is never disabled.

A short-lived server certificate respects Apple's TLS certificate requirements:
https://support.apple.com/en-us/103769
