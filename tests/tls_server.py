"""HTTPS and SOCKS fixtures with temporary certificates and no external services."""

import http.server
import os
from pathlib import Path
import select
import socket
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        data = bytes((index * 31 + 7) & 255 for index in range(4096))
        first, last = 0, len(data) - 1
        requested = self.headers.get("Range")
        if requested:
            start, end = requested.removeprefix("bytes=").split("-", 1)
            if start:
                first = int(start)
                last = min(int(end), last) if end else last
            else:
                first = max(0, len(data) - int(end))
        body = data[first:last + 1]
        self.send_response(206 if requested else 200)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("ETag", '"fixture"')
        if requested:
            self.send_header("Content-Range", f"bytes {first}-{last}/{len(data)}")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class SocksHandler(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(10)
        version, count = self.rfile.read(2)
        if version != 5 or 0 not in self.rfile.read(count):
            return
        self.connection.sendall(b"\x05\x00")
        # SOCKS5h must send the hostname, leaving resolution to this proxy.
        if self.rfile.read(4) != b"\x05\x01\x00\x03":
            return
        length = self.rfile.read(1)[0]
        host = self.rfile.read(length)
        port = int.from_bytes(self.rfile.read(2), "big")
        if host != b"proxy-only.invalid" or port != self.server.target_port:
            return
        self.server.used.set()
        with socket.create_connection(("127.0.0.1", port), timeout=10) as upstream:
            self.connection.sendall(b"\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00")
            while True:
                ready, _, _ = select.select([self.connection, upstream], [], [], 10)
                if not ready:
                    return
                for source in ready:
                    data = source.recv(65536)
                    if not data:
                        return
                    destination = upstream if source is self.connection else self.connection
                    destination.sendall(data)


class SocksServer(socketserver.ThreadingTCPServer):
    daemon_threads = True


def main():
    env = {key: value for key, value in os.environ.items()
           if key.lower() not in {"http_proxy", "https_proxy", "all_proxy", "no_proxy"}}
    env["no_proxy"] = "*"
    with tempfile.TemporaryDirectory(prefix="karu-tls-") as directory:
        subprocess.run([sys.argv[1], "--init", directory], env=env, check=True)
        root = Path(directory)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(root / "trusted.pem", root / "trusted.key")
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        proxy = SocksServer(("127.0.0.1", 0), SocksHandler)
        proxy.target_port = server.server_port
        proxy.used = threading.Event()
        threads = [threading.Thread(target=item.serve_forever, daemon=True)
                   for item in (server, proxy)]
        for thread in threads:
            thread.start()
        try:
            result = subprocess.run([sys.argv[1], str(server.server_port), directory,
                                     str(proxy.server_address[1])], env=env, timeout=60)
            return result.returncode or (0 if proxy.used.is_set() else 1)
        finally:
            for item in (server, proxy):
                item.shutdown()
                item.server_close()
            for thread in threads:
                thread.join()


if __name__ == "__main__":
    sys.exit(main())
