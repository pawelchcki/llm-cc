Local-only TLS fixture. The intentionally public test key authenticates localhost
and must never be used by a deployed service. `server.pem` is self-signed with a
DNS SAN for localhost; `untrusted.pem` is an unrelated CA. The fixture expires in
September 2036. The integration test binds an ephemeral loopback port, explicitly
selects its CA, and verifies trust, hostname, redirects and range resumption.
