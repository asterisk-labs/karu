"""Loopback HTTP fixture for the compiled Karu integration test."""

from __future__ import annotations

import email.utils
import http.server
import subprocess
import sys
import threading
import time
import urllib.parse


DATA = bytes((index * 31 + 7) & 0xFF for index in range(4096))
LARGE_DATA = bytes((index * 31 + 7) & 0xFF for index in range(128 * 1024))


class RedirectTargetHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    leaked_header = threading.Event()

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        if self.headers.get("X-Karu-Secret") is not None:
            RedirectTargetHandler.leaked_header.set()
        range_header = self.headers.get("Range", "")
        if not range_header.startswith("bytes=") or "-" not in range_header:
            self.send_error(400)
            return
        first_text, last_text = range_header[6:].split("-", 1)
        first = int(first_text)
        last = min(int(last_text), len(DATA) - 1)
        body = DATA[first : last + 1]
        self.send_response(206)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Content-Range", f"bytes {first}-{last}/{len(DATA)}")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, _format: str, *_args: object) -> None:
        pass


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    retries = 0
    scatter_retries = 0
    large_error_retries = 0
    large_error_connection = None
    large_size_retries = 0
    large_size_connection = None
    size_connection = None
    region_retries = 0
    late_region_retries = 0
    same_region_requests = 0
    retry_date_requests = 0
    cancel_started = threading.Event()
    cancel_range_ok = False
    redirect_target_port = 0

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
        if path == "/cancel-ready":
            if not Handler.cancel_started.is_set() or not Handler.cancel_range_ok:
                self.reply(425, b"not ready")
                return
            self.reply(206, DATA[:1], Content_Range=f"bytes 0-0/{len(DATA)}")
            return
        if path == "/redirect-leak-status":
            leaked = b"\x01" if RedirectTargetHandler.leaked_header.is_set() else b"\x00"
            self.reply(206, leaked, Content_Range="bytes 0-0/1")
            return
        if path.endswith("/missing"):
            self.reply(404, b"missing")
            return
        if path == "/unauthorized":
            self.reply(401, b"unauthorized")
            return
        if path == "/user-agent" and not self.headers.get("User-Agent", "").startswith("karu/"):
            self.reply(400, b"missing karu user agent")
            return
        if path == "/retry" and Handler.retries == 0:
            Handler.retries += 1
            self.reply(503, b"try again", Retry_After="0")
            return
        if path == "/scatter-retry" and Handler.scatter_retries == 0:
            Handler.scatter_retries += 1
            self.reply(503, b"try again", Retry_After="0")
            return
        if path == "/retry-after-deadline":
            self.reply(503, b"try later", Retry_After="60")
            return
        if path == "/retry-date-deadline" and Handler.retry_date_requests == 0:
            Handler.retry_date_requests += 1
            retry_at = email.utils.formatdate(time.time() + 60, usegmt=True)
            self.reply(503, b"try later", Retry_After=retry_at)
            return
        if path == "/large-error-retry":
            if Handler.large_error_retries == 0:
                Handler.large_error_retries += 1
                Handler.large_error_connection = self.connection
                self.reply(503, b"x" * 8192, Retry_After="0")
                return
            if self.connection is not Handler.large_error_connection:
                self.reply(409, b"connection was not reused")
                return
        if path == "/large-size-retry":
            if Handler.large_size_retries == 0:
                Handler.large_size_retries += 1
                Handler.large_size_connection = self.connection
                self.reply(503, b"x" * 8192, Retry_After="0")
                return
            if self.connection is not Handler.large_size_connection:
                self.reply(409, b"connection was not reused")
                return
        if path == "/size-reuse":
            if Handler.size_connection is None:
                Handler.size_connection = self.connection
            elif self.connection is not Handler.size_connection:
                self.reply(409, b"connection was not reused")
                return
        if path == "/redirect":
            self.reply(302, Location="/object")
            return
        if path == "/redirect-cross-origin":
            self.reply(
                302,
                Location=f"http://127.0.0.1:{Handler.redirect_target_port}/object",
            )
            return
        if path == "/redirect-same-origin-header":
            self.reply(
                302,
                Location=f"http://127.0.0.1:{self.server.server_port}/require-secret",
            )
            return
        if path == "/require-secret" and self.headers.get("X-Karu-Secret") != "sentinel":
            self.reply(400, b"missing redirect header")
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
        if path == "/bucket/late-region" and Handler.late_region_retries == 0:
            Handler.late_region_retries += 1
            self.reply(
                400,
                b"<Error><Code>AuthorizationHeaderMalformed</Code><Message>"
                + b"x" * 1024
                + b"</Message><Region>us-west-2</Region></Error>",
            )
            return
        if path == "/bucket/same-region" and Handler.same_region_requests == 0:
            Handler.same_region_requests += 1
            self.reply(403, b"forbidden", X_Amz_Bucket_Region="us-east-1")
            return
        if path == "/bucket/credential-refresh":
            authorization = self.headers.get("Authorization", "")
            if authorization.startswith("Bearer "):
                current = authorization.removeprefix("Bearer ")
            else:
                current = self.headers.get("x-amz-security-token")
            if current != "fresh":
                if authorization.startswith("AWS4-HMAC-SHA256 "):
                    self.reply(403, b"<Error><Code>ExpiredToken</Code></Error>")
                else:
                    self.reply(401, b"expired")
                return
        if path == "/container/credential-refresh":
            if self.headers.get("Authorization") != "Bearer fresh":
                self.reply(401, b"expired")
                return
        if path == "/account/product/credential-refresh":
            if self.headers.get("x-amz-security-token") != "fresh":
                self.reply(401, b"expired")
                return
        if path.endswith("/precondition") and self.headers.get("If-Match") != '"v1"':
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
        if path == "/oversized-size-body":
            self.reply(206, DATA[:2], Content_Range=f"bytes 0-0/{len(DATA)}")
            return
        if path == "/missing-size-body":
            self.reply(206, Content_Range=f"bytes 0-0/{len(DATA)}")
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
        if path == "/subfile-coalesced" and range_header != "bytes=176-343":
            self.reply(409, b"subfile requests were not coalesced")
            return
        if path == "/submit-separate" and range_header not in ("bytes=0-23", "bytes=24-47"):
            self.reply(409, b"submit override did not disable coalescing")
            return
        if path == "/scatter-chunks" and range_header != "bytes=1000-79999":
            self.reply(409, b"scatter requests were not coalesced")
            return
        if path == "/cancel-coalesced":
            Handler.cancel_range_ok = range_header == "bytes=576-799"
            Handler.cancel_started.set()
            if not Handler.cancel_range_ok:
                self.reply(409, b"cancel requests were not coalesced")
                return
            time.sleep(4)
        if path == "/slow":
            time.sleep(4)
        first_text, last_text = range_header[6:].split("-", 1)
        first = int(first_text)
        source = LARGE_DATA if path == "/scatter-chunks" else DATA
        last = min(int(last_text), len(source) - 1)
        if first >= len(source):
            self.reply(416, Content_Range=f"bytes */{len(source)}")
            return
        body = source[first : last + 1]
        if path == "/truncated":
            self.send_response(206)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Content-Range", f"bytes {first}-{last}/{len(DATA)}")
            self.end_headers()
            self.wfile.write(body[: max(1, len(body) // 2)])
            self.wfile.flush()
            self.close_connection = True
            return
        if path == "/chunked":
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {first}-{last}/{len(DATA)}")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            at = 0
            for chunk_size in (1, 17, 3, 64, 5, 257):
                if at == len(body):
                    break
                chunk = body[at : at + chunk_size]
                self.wfile.write(f"{len(chunk):x}\r\n".encode("ascii"))
                self.wfile.write(chunk + b"\r\n")
                at += len(chunk)
            if at < len(body):
                chunk = body[at:]
                self.wfile.write(f"{len(chunk):x}\r\n".encode("ascii"))
                self.wfile.write(chunk + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
            return
        reported_first = 0 if path == "/bad-range" else first
        reported_last = reported_first + len(body) - 1
        if path == "/bad-range-end":
            reported_last += 1
        if path == "/scatter-chunks":
            self.send_response(206)
            self.send_header("Content-Length", str(len(body)))
            self.send_header(
                "Content-Range", f"bytes {reported_first}-{reported_last}/{len(source)}"
            )
            self.end_headers()
            at = 0
            chunk_sizes = (1, 16383, 7, 32768)
            chunk = 0
            while at < len(body):
                end = min(len(body), at + chunk_sizes[chunk % len(chunk_sizes)])
                self.wfile.write(body[at:end])
                self.wfile.flush()
                at = end
                chunk += 1
            return
        self.reply(
            206,
            body,
            Content_Range=f"bytes {reported_first}-{reported_last}/{len(source)}",
        )

    def log_message(self, _format: str, *_args: object) -> None:
        pass


def main() -> int:
    if len(sys.argv) != 2:
        return 2
    target = http.server.ThreadingHTTPServer(("127.0.0.1", 0), RedirectTargetHandler)
    Handler.redirect_target_port = target.server_address[1]
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    target_worker = threading.Thread(target=target.serve_forever, daemon=True)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    target_worker.start()
    worker.start()
    try:
        completed = subprocess.run(
            [sys.argv[1], str(server.server_address[1])], check=False
        )
        return completed.returncode
    finally:
        server.shutdown()
        target.shutdown()
        worker.join()
        target_worker.join()
        server.server_close()
        target.server_close()


if __name__ == "__main__":
    raise SystemExit(main())
