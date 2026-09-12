"""Loopback credential endpoints for the compiled Karu credential test.

Every route answers deterministically. Timestamps are computed per request so
that "valid" credentials are always in the future and "expired" ones always in
the past, no matter when the suite runs.
"""

from __future__ import annotations

import calendar
import http.server
import json
import subprocess
import sys
import threading
import time
import urllib.parse


DATA = bytes((index * 31 + 7) & 0xFF for index in range(4096))

_lock = threading.Lock()
_requests: list[dict[str, object]] = []


def iso8601(offset: int) -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() + offset))


def epoch(offset: int) -> int:
    return int(calendar.timegm(time.gmtime())) + offset


def sts_xml(key: str, secret: str, token: str, expiration: str) -> bytes:
    parts = ["<AssumeRoleWithWebIdentityResponse><AssumeRoleWithWebIdentityResult>",
             "<Credentials>"]
    if key:
        parts.append(f"<AccessKeyId>{key}</AccessKeyId>")
    if secret:
        parts.append(f"<SecretAccessKey>{secret}</SecretAccessKey>")
    if token:
        parts.append(f"<SessionToken>{token}</SessionToken>")
    if expiration:
        parts.append(f"<Expiration>{expiration}</Expiration>")
    parts.append("</Credentials></AssumeRoleWithWebIdentityResult>"
                 "</AssumeRoleWithWebIdentityResponse>")
    return "".join(parts).encode()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    ipv6_redirect = "http://[::1]:9/object"

    def send_body(self, status: int, body: bytes, content_type: str, **headers: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for name, value in headers.items():
            self.send_header(name.replace("_", "-"), value)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def send_json(self, status: int, payload: object) -> None:
        self.send_body(status, json.dumps(payload).encode(), "application/json")

    def send_text(self, status: int, text: str) -> None:
        self.send_body(status, text.encode(), "text/plain")

    def record(self, body: bytes) -> None:
        with _lock:
            _requests.append({
                "path": self.path,
                "method": self.command,
                "headers": {name.lower(): value for name, value in self.headers.items()},
                "body": body.decode("utf-8", "replace"),
            })

    def route(self, body: bytes) -> None:
        split = urllib.parse.urlsplit(self.path)
        path = split.path
        self.record(body)

        if path == "/_requests":
            with _lock:
                self.send_json(200, _requests)
            return
        if path == "/_reset":
            with _lock:
                _requests.clear()
            self.send_json(200, {"ok": True})
            return
        if path == "/_count":
            # How many times one route has been asked for. A test that must
            # prove a route was NOT taken reads this before and after, which
            # stays correct however the suites are ordered.
            wanted = urllib.parse.parse_qs(split.query).get("path", [""])[0]
            with _lock:
                hits = sum(1 for entry in _requests
                           if urllib.parse.urlsplit(str(entry["path"])).path == wanted)
            self.send_json(200, {"path": wanted, "hits": hits})
            return

        # OAuth token endpoints, shared by GCS service accounts, GCS authorized
        # users, GCS external accounts and Azure service principals.
        if path == "/oauth/ok":
            self.send_json(200, {"access_token": "oauth-access-token",
                                 "expires_in": 3600, "token_type": "Bearer"})
            return
        if path == "/oauth/short":
            self.send_json(200, {"access_token": "short-lived-token", "expires_in": 10})
            return
        if path == "/oauth/empty-token":
            self.send_json(200, {"access_token": "", "expires_in": 3600})
            return
        if path == "/oauth/quoted-expiry":
            self.send_json(200, {"access_token": "quoted-expiry-token", "expires_in": "900"})
            return
        if path == "/oauth/no-token":
            self.send_json(200, {"token_type": "Bearer"})
            return
        if path == "/oauth/denied":
            self.send_json(400, {"error": "invalid_grant"})
            return

        # AWS AssumeRoleWithWebIdentity.
        if path == "/aws/sts/ok":
            self.send_body(200, sts_xml("ASIASTSFIXTURE", "sts-secret", "sts-session",
                                        iso8601(3600)), "text/xml")
            return
        if path == "/aws/sts/incomplete":
            self.send_body(200, sts_xml("ASIASTSFIXTURE", "", "sts-session", iso8601(3600)),
                           "text/xml")
            return
        if path == "/aws/sts/expired":
            self.send_body(200, sts_xml("ASIASTSFIXTURE", "sts-secret", "sts-session",
                                        iso8601(-3600)), "text/xml")
            return
        if path == "/aws/sts/unterminated":
            self.send_body(200, b"<Credentials><AccessKeyId>ASIASTSFIXTURE</AccessKeyId>"
                                b"<SecretAccessKey>sts-secret", "text/xml")
            return
        if path == "/aws/sts/denied":
            self.send_body(403, b"<Error><Code>AccessDenied</Code></Error>", "text/xml")
            return

        # AWS container credentials (ECS and EKS pod identity).
        if path == "/aws/container/ok":
            self.send_json(200, {"AccessKeyId": "ASIACONTAINER",
                                 "SecretAccessKey": "container-secret",
                                 "Token": "container-token",
                                 "Expiration": iso8601(3600)})
            return
        if path == "/aws/container/incomplete":
            self.send_json(200, {"AccessKeyId": "ASIACONTAINER",
                                 "SecretAccessKey": "container-secret",
                                 "Expiration": iso8601(3600)})
            return
        if path == "/aws/container/expired":
            self.send_json(200, {"AccessKeyId": "ASIACONTAINER",
                                 "SecretAccessKey": "container-secret",
                                 "Token": "container-token",
                                 "Expiration": iso8601(-3600)})
            return
        if path == "/aws/container/session-token-field":
            self.send_json(200, {"AccessKeyId": "ASIACONTAINER",
                                 "SecretAccessKey": "container-secret",
                                 "SessionToken": "container-session-token",
                                 "Expiration": iso8601(3600)})
            return
        if path == "/aws/container/key-only":
            self.send_json(200, {"AccessKeyId": "ASIACONTAINER",
                                 "Expiration": iso8601(3600)})
            return
        if path == "/aws/container/denied":
            self.send_json(401, {"message": "not authorized"})
            return

        # GCS compute metadata.
        if path == "/gcs/metadata/ok":
            self.send_json(200, {"access_token": "gce-metadata-token", "expires_in": 1800})
            return
        if path == "/gcs/metadata/no-expires-in":
            self.send_json(200, {"access_token": "gce-metadata-token"})
            return
        if path == "/gcs/metadata/no-token":
            self.send_json(200, {"expires_in": 1800})
            return
        if path == "/gcs/metadata/denied":
            self.send_json(500, {"error": "metadata unavailable"})
            return

        # GCS external account subject tokens.
        if path == "/gcs/subject/text":
            self.send_text(200, "  subject-token-from-url  ")
            return
        if path == "/gcs/subject/json":
            self.send_json(200, {"id_token": "subject-token-from-field"})
            return
        if path == "/gcs/subject/empty-field":
            self.send_json(200, {"id_token": ""})
            return
        if path == "/gcs/subject/denied":
            self.send_json(500, {"error": "no subject"})
            return

        # GCS service account impersonation.
        if path == "/gcs/impersonate/ok":
            self.send_json(200, {"accessToken": "impersonated-token",
                                 "expireTime": iso8601(3600)})
            return
        if path == "/gcs/impersonate/expired":
            self.send_json(200, {"accessToken": "impersonated-token",
                                 "expireTime": iso8601(-3600)})
            return
        if path == "/gcs/impersonate/no-token":
            self.send_json(200, {"expireTime": iso8601(3600)})
            return
        if path == "/gcs/impersonate/denied":
            self.send_json(403, {"error": "impersonation denied"})
            return

        # Azure App Service identity and IMDS share this response shape.
        if path == "/azure/identity/ok":
            self.send_json(200, {"access_token": "azure-identity-token",
                                 "expires_on": epoch(3600)})
            return
        if path == "/azure/identity/no-expiry":
            self.send_json(200, {"access_token": "azure-identity-token"})
            return
        if path == "/azure/identity/no-token":
            self.send_json(200, {"resource": "https://storage.azure.com/"})
            return
        if path == "/azure/identity/denied":
            self.send_json(403, {"error": "identity denied"})
            return

        # Plain HTTP behaviours used by the transport coverage cases.
        if path == "/oversized":
            # Past the 1 MiB ceiling collect_body enforces on credential replies.
            self.send_body(200, b"x" * (1024 * 1024 + 4096), "application/json")
            return
        if path == "/size/length-only":
            self.send_body(200, DATA, "application/octet-stream")
            return
        # Cross-origin or malformed redirect targets. None of them is ever
        # contacted: the header check rejects the redirect before libcurl opens
        # a connection.
        if path == "/redirect/ipv6":
            self.send_body(302, b"", "text/plain", Location=Handler.ipv6_redirect)
            return
        if path == "/redirect/ipv6-unclosed":
            self.send_body(302, b"", "text/plain", Location="http://[::1:9/object")
            return
        if path == "/redirect/ipv6-trailing":
            self.send_body(302, b"", "text/plain", Location="http://[::1]x/object")
            return
        if path == "/redirect/bad-port":
            self.send_body(302, b"", "text/plain", Location="http://127.0.0.1:99999/object")
            return
        if path == "/redirect/empty-port":
            self.send_body(302, b"", "text/plain", Location="http://127.0.0.1:/object")
            return
        if path == "/redirect/scheme":
            self.send_body(302, b"", "text/plain", Location="gopher://127.0.0.1/object")
            return
        if path == "/redirect/no-host":
            self.send_body(302, b"", "text/plain", Location="http://:8080/object")
            return
        if path == "/redirect/empty-location":
            self.send_body(302, b"", "text/plain", Location="")
            return
        if path == "/range":
            header = self.headers.get("Range", "")
            if not header.startswith("bytes=") or "-" not in header:
                self.send_text(400, "missing range")
                return
            first_text, last_text = header[6:].split("-", 1)
            first = int(first_text)
            last = min(int(last_text), len(DATA) - 1)
            if first >= len(DATA):
                self.send_body(416, b"", "text/plain",
                               Content_Range=f"bytes */{len(DATA)}")
                return
            body = DATA[first:last + 1]
            self.send_body(206, body, "application/octet-stream",
                           Content_Range=f"bytes {first}-{last}/{len(DATA)}")
            return

        if path.startswith("/status/"):
            self.send_text(int(path.rsplit("/", 1)[1]), "status fixture")
            return

        self.send_text(404, "no such credential fixture")

    def read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length", "0") or "0")
        return self.rfile.read(length) if length > 0 else b""

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self.route(b"")

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self.route(self.read_body())

    def do_PUT(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self.route(self.read_body())

    def log_message(self, _format: str, *_args: object) -> None:
        pass


def main() -> int:
    if len(sys.argv) != 2:
        return 2
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.request_queue_size = 256
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        completed = subprocess.run(
            [sys.argv[1], str(server.server_address[1])], check=False
        )
        return completed.returncode
    finally:
        server.shutdown()
        worker.join()
        server.server_close()


if __name__ == "__main__":
    raise SystemExit(main())
