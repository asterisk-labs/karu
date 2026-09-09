"""Loopback HTTP fixture for the compiled Karu integration test."""

from __future__ import annotations

import http.server
import subprocess
import sys
import threading
import urllib.parse


DATA = bytes((index * 31 + 7) & 0xFF for index in range(4096))


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    retries = 0
    region_retries = 0
    same_region_requests = 0

    def reply(self, status: int, body: bytes = b"", **headers: str) -> None:
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        for name, value in headers.items():
            self.send_header(name.replace("_", "-"), value)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        path = urllib.parse.urlsplit(self.path).path
        if path == "/missing":
            self.reply(404, b"missing")
            return
        if path == "/unauthorized":
            self.reply(401, b"unauthorized")
            return
        if path == "/retry" and Handler.retries == 0:
            Handler.retries += 1
            self.reply(503, b"try again", Retry_After="0")
            return
        if path == "/redirect":
            self.reply(302, Location="/object")
            return
        if path == "/redirect-ftp":
            self.reply(302, Location="ftp://127.0.0.1:1/object")
            return
        if path == "/signed-redirect" or path == "/bucket/signed-redirect":
            self.reply(302, b"x" * 1024, Location="/object")
            return
        if path == "/bucket/region" and Handler.region_retries == 0:
            Handler.region_retries += 1
            self.reply(
                400,
                b"<Error><Code>AuthorizationHeaderMalformed</Code>"
                b"<Region>us-west-2</Region></Error>",
            )
            return
        if path == "/bucket/same-region" and Handler.same_region_requests == 0:
            Handler.same_region_requests += 1
            self.reply(403, b"forbidden", X_Amz_Bucket_Region="us-east-1")
            return
        if path == "/bucket/credential-refresh":
            if self.headers.get("Authorization") != "Bearer fresh":
                self.reply(401, b"expired")
                return
        if path == "/precondition" and self.headers.get("If-Match") != '"v1"':
            self.reply(412, b"changed")
            return
        if path == "/ignore-range":
            self.reply(200, DATA)
            return
        if path == "/no-content":
            self.reply(204)
            return
        if path == "/unknown-size":
            self.reply(206, DATA[:1], Content_Range="bytes 0-0/*")
            return
        if path == "/invalid-empty-size":
            self.reply(416, Content_Range="garbage/0")
            return
        if path == "/empty":
            self.reply(416, Content_Range="bytes */0")
            return

        range_header = self.headers.get("Range", "")
        if not range_header.startswith("bytes=") or "-" not in range_header:
            self.reply(400, b"missing range")
            return
        first_text, last_text = range_header[6:].split("-", 1)
        first = int(first_text)
        last = min(int(last_text), len(DATA) - 1)
        if first >= len(DATA):
            self.reply(416, Content_Range=f"bytes */{len(DATA)}")
            return
        body = DATA[first : last + 1]
        reported_first = 0 if path == "/bad-range" else first
        reported_last = reported_first + len(body) - 1
        if path == "/bad-range-end":
            reported_last += 1
        self.reply(
            206,
            body,
            Content_Range=f"bytes {reported_first}-{reported_last}/{len(DATA)}",
        )

    def log_message(self, _format: str, *_args: object) -> None:
        pass


def main() -> int:
    if len(sys.argv) != 2:
        return 2
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
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
