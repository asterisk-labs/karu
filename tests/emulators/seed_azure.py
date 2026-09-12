"""Upload the fixture blob into Azurite.

The request is signed here, from the Shared Key specification, with no help from
Karu. Azurite verifies it. Two independent implementations therefore have to
agree with the emulator before any test runs, which is the whole point of
testing against an emulator rather than against a fixture of our own.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import os
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from email.utils import formatdate

# Azurite's documented well-known development account.
ACCOUNT = "devstoreaccount1"
KEY = ("Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq"
       "/K1SZFPTOtr/KBHBeksoGMGw==")
VERSION = "2021-08-06"
CONTAINER = "karu"
BLOB = "fixture.bin"


def endpoint() -> str:
    port = os.environ.get("KARU_TEST_AZURE_PORT", "10000")
    return f"http://127.0.0.1:{port}/{ACCOUNT}"


def string_to_sign(method: str, path: str, query: dict[str, str],
                   headers: dict[str, str], length: int, content_type: str) -> str:
    # VERB, Content-Encoding, Content-Language, Content-Length, Content-MD5,
    # Content-Type, Date, If-Modified-Since, If-Match, If-None-Match,
    # If-Unmodified-Since, Range: twelve fields, each terminated by a newline.
    # A Content-Length of zero signs as an empty string.
    fields = [method, "", "", str(length) if length else "", "", content_type,
              "", "", "", "", "", ""]
    canonical_headers = "".join(
        f"{name.lower()}:{value}\n"
        for name, value in sorted(headers.items())
        if name.lower().startswith("x-ms-")
    )
    # Azurite serves a path-style endpoint, so the account appears once as the
    # signing prefix and again inside the request path.
    canonical_resource = f"/{ACCOUNT}/{ACCOUNT}{path}"
    for name in sorted(query):
        canonical_resource += f"\n{name.lower()}:{query[name]}"
    return "\n".join(fields) + "\n" + canonical_headers + canonical_resource


def call(method: str, path: str, query: dict[str, str] | None = None,
         body: bytes = b"", extra: dict[str, str] | None = None) -> tuple[int, bytes]:
    query = query or {}
    extra = extra or {}
    headers = {"x-ms-date": formatdate(usegmt=True), "x-ms-version": VERSION, **extra}
    signature = hmac.new(
        base64.b64decode(KEY),
        string_to_sign(method, path, query, headers, len(body),
                       extra.get("Content-Type", "")).encode("utf-8"),
        hashlib.sha256,
    ).digest()
    headers["Authorization"] = f"SharedKey {ACCOUNT}:{base64.b64encode(signature).decode()}"
    if body:
        headers["Content-Length"] = str(len(body))
    url = endpoint() + path + ("?" + urllib.parse.urlencode(query) if query else "")
    request = urllib.request.Request(url, data=body or None, method=method, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            return response.status, b""
    except urllib.error.HTTPError as error:
        return error.code, error.read()[:400]
    except OSError as error:
        print(f"  {method} {url}: {error}", file=sys.stderr)
        return 0, b""


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    payload = subprocess.run([sys.executable, os.path.join(here, "fixture.py")],
                             check=True, capture_output=True).stdout

    status, detail = call("PUT", f"/{CONTAINER}", {"restype": "container"})
    # 409 means the container survived a previous run, which is fine.
    if status not in (201, 409):
        print(f"crear contenedor: {status} {detail.decode('utf-8', 'replace')}", file=sys.stderr)
        return 1

    status, detail = call("PUT", f"/{CONTAINER}/{BLOB}", body=payload,
                          extra={"x-ms-blob-type": "BlockBlob",
                                 "Content-Type": "application/octet-stream"})
    if status != 201:
        print(f"subir blob: {status} {detail.decode('utf-8', 'replace')}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
