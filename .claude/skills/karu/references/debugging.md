# Debugging

Messages below were captured from karu 0.2.3 (macOS, libcurl 8.7.1) with the programs in
`c-api.md`, the loopback fixture `tests/http_server.py`, and real reads from Hugging Face
and a public S3 bucket. Paths, ports and libcurl wording vary between machines; match on
the stable part of each message.

## Contents

1. Reading a failure
2. Resolve errors
3. Configuration errors
4. Submit errors
5. Read failures: local files
6. Read failures: HTTP and cloud
7. Credential problems
8. TLS, proxies and redirects
9. Slow reads, hangs and timeouts
10. Threads and fork
11. Reproducing and reporting

## 1. Reading a failure

- Separate the call status from the read status. `karu_batch_next` returning `KARU_OK`
  only means a completion is ready; `done.status` (C++ `event.status`) is the read.
- Read `karu_last_error()` immediately, on the same thread. The C++ facade copies it into
  `Error::message` and `BatchEvent::message`.
- `karu_status_string` names the category; the message names the object first, then the
  cause. HTTP URLs have user information and query strings replaced by `<redacted>`.
- A `fetch` reports only the first failure. Switch to `submit` plus `next` to see every
  failed read.

## 2. Resolve errors

| Input | Status | Message | Fix |
| --- | --- | --- | --- |
| `""` | `malformed URI` | `empty URI` | pass a path |
| `/vsizip/archive.zip/a.tif` | `unsupported virtual filesystem` | `unsupported virtual filesystem '/vsizip/'; Karu supports /vsisubfile/, /vsicurl/, /vsis3/, /vsigs/, /vsiaz/, /vsiadls/, /vsihf/ and /vsisource/ (no *_streaming aliases)` | address the member with `/vsisubfile/OFFSET_SIZE,<archive>` |
| `/vsis3_streaming/bucket/key` | `unsupported virtual filesystem` | same, naming `'/vsis3_streaming/'` | use `/vsis3/` |
| `s3://bucket` | `malformed URI` | `expected s3://bucket/key in 'bucket'` | include a key |
| `ftp://host/object`, `S3://b/k` | `malformed URI` | `unknown scheme in 'ftp://host/object'` | supported schemes are lowercase |
| `/vsicurl/s3://bucket/key` | `malformed URI` | `/vsicurl/ target must be an HTTP URL` | use `/vsis3/bucket/key` |
| `/vsisubfile/1_2` | `malformed URI` | `/vsisubfile/ wants OFFSET[_LENGTH],<uri>` | add `,<inner path>` |
| `/vsisubfile/abc,/tmp/a` | `malformed URI` | `'abc' is not a byte count` | decimal numbers only |
| `https://example.test/a#bytes=10-4` | `malformed URI` | `#bytes= ends before it starts` | `FIRST-LAST`, inclusive |
| `source://account/product` | `malformed URI` | `expected source://account/product/key in 'product'` | account, product and key |
| `hf://datasets/org/repo` | `malformed URI` | `expected hf://owner/name/path in 'repo'` | add the file path |

## 3. Configuration errors

At `set` time (`KARU_ERR_CONFIG`):

| Message | Cause |
| --- | --- |
| `unknown Karu option 'KARU_THREADS'` | misspelled or unsupported name; the list is `src/config_options.hpp` |
| `KARU_CONCURRENCY is client-wide and cannot be path-specific` | `KARU_*` in a path rule, `KARU_HTTP_*` included |
| `clearing a credentials provider requires null user_data and release` | clearing with leftover arguments |

At `karu_client_create` (`KARU_ERR_CONFIG`), including values inherited from the
environment:

| Message | Fix |
| --- | --- |
| `KARU_CONCURRENCY must be an integer in [1, 4096]` | the ranges are in `configuration.md` section 4 |
| `KARU_MAX_ATTEMPTS must be an integer in [1, 16]` | |
| `AWS_NO_SIGN_REQUEST must be YES/NO, TRUE/FALSE, ON/OFF, or 1/0` | also `AWS_HTTPS`, `AWS_VIRTUAL_HOSTING`, the `*_METADATA_DISABLED` and `*_NO_SIGN_REQUEST` options |
| `KARU_HTTP_VERSION must be AUTO, 1.1, 2TLS, 2, or 2PRIOR_KNOWLEDGE` | |
| `AWS_REQUEST_PAYER must be 'requester'` | |
| `AWS_METADATA_SERVICE_TIMEOUT must be an integer in [1, 60]` | |

When client creation fails only in some environments, print the offending variable:
the builder took it from the environment of the process.

At read time (`KARU_ERR_CONFIG` on each completion):

| Message | Cause |
| --- | --- |
| `KARU_HTTP_HEADERS cannot override Karu-managed header 'Range'` | `Host`, `Range`, generated headers, and for cloud or Hugging Face also `Authorization`, `Date`, `If-Match`, `x-amz-*`, `x-goog-*`, `x-ms-*` |
| `/vsiaz/container/blob: Azure needs AZURE_STORAGE_ACCOUNT or an explicit endpoint` | Azure without an account or endpoint, anonymous reads included |
| `AWS_S3_ENDPOINT must use HTTP or HTTPS`, `endpoint must not contain user information`, `... cannot contain a query` | malformed endpoint option |

## 4. Submit errors

All `KARU_ERR_INVALID`, and nothing was submitted:

| Message | Cause |
| --- | --- |
| `karu_client_submit: request 0 is empty` | `length == 0` |
| `karu_client_submit: request 0 does not have a finite length` | `length == KARU_TO_END`; ask `karu_client_size` first |
| `karu_client_submit: request 0 has an invalid ETag` | CR or LF in `if_match` |
| `karu_client_fetch: request 0 has no destination buffer` | `fetch` requires caller buffers |
| `karu_client_submit_with: submit options structure is too small` | `struct_size` not set; use `KARU_SUBMIT_OPTIONS_INIT` |
| `karu_client_submit_with: coalesce_limit must be at least 1` | zero limit, parts or amplification |
| `read has no object`, `read has an empty destination` | C++ `Read` without `object` or `destination` |

## 5. Read failures: local files

| Status | Message | Meaning |
| --- | --- | --- |
| `range past end of object`, `got=6` | `/data/a.bin: object ended at byte 4096 while reading [4090, +16)` | the file ends inside the request; `got` bytes are valid |
| `range past end of object`, `got=0` | `/data/a.bin: object ended at byte 5000 while reading [5000, +16)` | the request starts at or past the end (the byte reported is the request start) |
| `range past end of object` | `/data/a.bin: range [40, +16) leaves the locator window` | offsets are relative to the `/vsisubfile/` window and must stay inside it |
| `unsupported virtual filesystem` | `/data/a.bin: ETag preconditions are not available for local files` | drop `if_match` for local paths |
| `file error` | `/data/a.bin.missing: No such file or directory` | path, permissions, or a relative path resolved from another working directory |

## 6. Read failures: HTTP and cloud

| Status | Message (loopback fixture) | Meaning |
| --- | --- | --- |
| `object not found` | `http://127.0.0.1:PORT/missing: HTTP 404: missing` | wrong key, bucket, revision or endpoint |
| `not authorized` | `http://127.0.0.1:PORT/unauthorized: HTTP 401: unauthorized` | credentials rejected, see section 7; S3 also answers 403 for a missing key when the caller lacks `s3:ListBucket` |
| `precondition failed` | `http://127.0.0.1:PORT/precondition: HTTP 412: changed` | the object no longer matches `if_match` |
| `precondition failed` | `http://127.0.0.1:PORT/ignores-if-match: the server ignored If-Match and sent ETag "e2", so the object no longer matches if_match` | the response ETag differs from the requested version; use the final object's ETag from `karu_client_stat` |
| `range past end of object`, `got=6` | `http://127.0.0.1:PORT/object: object ended at byte 4096 while reading [4090, +16)` | short object; the prefix is valid |
| `range past end of object` | `http://127.0.0.1:PORT/object: HTTP 416` | the request starts past the end |
| `http error` | `.../bad-range: Content-Range [0, 15] does not satisfy requested [100, 115]` | a proxy or server returned other bytes; nothing was delivered |
| `http error` | `.../ignore-range: server ignored Range; refusing to discard 4000 bytes (limit 1024)` | the server does not support ranges; raise `KARU_RANGE_FALLBACK_LIMIT` only for small objects |
| `http error` | `.../bucket/gzip-stored: the server decoded an object stored with Content-Encoding gzip (GCS decompressive transcoding), ...` | a GCS object stored gzip-encoded was read over plain HTTPS; use `gs://`, or send `Accept-Encoding: gzip` with `KARU_HTTP_HEADERS` |
| `http error` | `.../transformed: the response carries Warning 214 (transformation applied), ...` | a proxy rewrote the body; read from the origin |
| `http error` | `.../bucket/signed-redirect: HTTP 302: ...` | a signed cloud request was redirected; fix the endpoint or region instead of following it |
| `network error` | `.../truncated: transfer closed with 500 bytes remaining to read` | the connection dropped mid-body on every attempt |
| `network error` | `http://127.0.0.1:9/object: Failed to connect to 127.0.0.1 port 9 after 0 ms: Couldn't connect to server` | nothing listening, firewall, or wrong endpoint |
| `network error` | `.../redirect-ftp: Protocol "ftp" disabled (in redirect)` | redirects are limited to HTTP and HTTPS |
| `timed out` | `.../retry-after-deadline: request timeout leaves no time for another attempt` | the next backoff or `Retry-After` does not fit `KARU_REQUEST_TIMEOUT` |
| `http error` (size) | `.../unknown-size: partial response has no valid object size` | the server returns `Content-Range: bytes 0-0/*`; store sizes elsewhere |

S3 specifics:

- `HTTP 403` with `SignatureDoesNotMatch`: wrong secret, a proxy that rewrites headers,
  or a custom endpoint that expects another addressing style (`AWS_VIRTUAL_HOSTING`).
  `RequestTimeTooSkewed`: fix the system clock.
- `HTTP 403 AccessDenied` on a public bucket while other credentials are present: the
  request was signed with an identity the bucket rejects. Set `AWS_NO_SIGN_REQUEST=YES`
  for that bucket with a path rule.
- `HTTP 301` or `400` naming another region is corrected automatically once per
  transfer; a failure that persists means the endpoint does not match the bucket's
  region or partition.
- `HTTP 400 ... RequestTimeout` is retried; if it keeps failing, lower concurrency or
  check for stalled uploads through a proxy.

Hugging Face and HTTP specifics:

- `cross-origin redirect blocked because KARU_HTTP_HEADERS is set`: verified against
  `hf://datasets/...`, whose files redirect to a CDN. Remove `KARU_HTTP_HEADERS` for that
  client, or use a second client without it.
- `HTTP 401` or `403` from the Hub for a private or gated repository: log in with
  `hf auth login`, or set `HF_TOKEN`; `karu_config_create_empty()` does not read the
  token cache.

## 7. Credential problems

| Message | Cause and fix |
| --- | --- |
| `/vsis3/bucket/key: no AWS credentials; set AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY, install a custom provider, or set AWS_NO_SIGN_REQUEST=YES for a public object` | nothing in the chain; for public data set `AWS_NO_SIGN_REQUEST=YES`; with `karu_config_create_empty()` only explicit settings count |
| `/vsigs/bucket/key: no GCS credentials; set GCS_ACCESS_TOKEN, configure GCS_HMAC_ACCESS_KEY_ID/GCS_HMAC_SECRET_ACCESS_KEY, install a custom provider, or set GCS_NO_SIGN_REQUEST=YES` | same for GCS |
| `AWS profile 'research' was not found` | `AWS_PROFILE` names a missing section; config sections are `[profile research]`, credentials sections `[research]` |
| `AWS chained AssumeRole profiles require a custom credential provider; ...` | `role_arn` without a web identity token file |
| `AWS IAM Identity Center profiles require a custom credential provider` | SSO profile; export short-lived keys or use a callback backed by the AWS SDK |
| `AWS credential_process exceeded the request timeout` (status `timed out`) | helper slower than `KARU_REQUEST_TIMEOUT`, or waiting for interactive login |
| `AWS credential_process exited with status 1`, `... returned more than 1 MiB`, `... response is incomplete` | run the command by hand; it must print JSON with `AccessKeyId` and `SecretAccessKey` |
| `AWS_CONTAINER_CREDENTIALS_FULL_URI must use HTTPS or a loopback/ECS host` | plain HTTP to another host is refused |
| `/vsisource/account/product/key: Source profile 'source-coop' was not found` | signing was requested (a `SOURCE_*` credential option or `SOURCE_NO_SIGN_REQUEST=NO`) but no profile exists; run `source-coop login` |
| `GOOGLE_APPLICATION_CREDENTIALS has unsupported type '...'` | only `service_account`, `authorized_user` and `external_account` |
| `set only one Azure managed-identity selector: object ID, client ID, or resource ID` | `AZURE_IMDS_CLIENT_ID` also defaults to `AZURE_CLIENT_ID` |
| `custom credential provider returned expired credentials` | callback `expires_at` in the past (seconds, not milliseconds) |
| `custom credential cache_prefix does not contain the requested path` | prefix for another bucket, or a partial segment |
| `<path>: custom credential provider returned credentials unavailable` | the callback returned a failure status |

Silent costs worth checking:

- Signed reads without credentials off cloud machines wait for metadata probes. Measured:
  a failing `s3://` size call took 1.03 s with discovery and 0.02 s with
  `AWS_EC2_METADATA_DISABLED=YES`. Failed lookups are not cached, so every transfer pays
  again. Disable metadata or set `*_NO_SIGN_REQUEST=YES` when that is the intent.
- Static and profile credentials are reloaded every 60 seconds; a slow
  `credential_process` without `Expiration` runs that often.

## 8. TLS, proxies and redirects

- `SSL certificate problem: unable to get local issuer certificate`: a corporate proxy or
  private CA. Set `KARU_HTTP_CA_BUNDLE` (or `CURL_CA_BUNDLE`, `SSL_CERT_FILE`) to a PEM
  bundle that includes it, or `KARU_HTTP_CA_PATH`. On Linux and macOS Karu otherwise uses
  the first system bundle it finds when the configuration is created.
- `CURLOPT_CAPATH` is not supported by every TLS backend (Schannel on Windows answers
  `CURLE_NOT_BUILT_IN`); prefer a bundle file there.
- Proxies: `KARU_HTTP_PROXY` and `KARU_HTTP_PROXY_CREDENTIALS`, or libcurl's
  `https_proxy`, `http_proxy` and `no_proxy`. `SOURCE_PROXY_URL` is not a proxy setting.
- HTTP/2 settings fail at setup (`could not configure HTTP request: CURLOPT_HTTP_VERSION: ...`,
  status `network error`) when libcurl was built without HTTP/2. Keep `1.1` there.
- `karu_http_backend()` prints the libcurl version and TLS library in use.

## 9. Slow reads, hangs and timeouts

- Throughput far below expectations with remote data: check that one client is reused,
  that reads are submitted as batches, and `karu_client_concurrency()`. Then compare the
  number of transfers with the number of reads (`read-path.md`, section 2).
- A read that fails only after about 120 seconds hit `KARU_REQUEST_TIMEOUT`; a stalled
  connection trips `KARU_LOW_SPEED_TIME` (60 s below 1024 bytes/s) first.
- Retry waits alone total 19 to 38 s for 8 attempts (0.6 to 1.2 s for 3), and can
  be longer with `Retry-After`. Failed requests take additional time. DNS and connection
  failures get at most 3 attempts, still subject to resolver and connection timeouts.
- `karu_batch_free` or a C++ `Batch` destructor blocks until workers release the batch's
  buffers. It can wait for a credential callback or `credential_process` that is already
  running, because Karu cannot interrupt either.
- A credential callback that calls back into the same client can deadlock the credential
  workers. Resolve credentials with the vendor SDK only.
- `KARU_ERR_INVALID` with `batches do not cross fork()` means the batch belongs to
  the parent process. Submit a new batch in the child; see section 10.

## 10. Threads and fork

- `karu_last_error()` is per thread. Reading it on another thread shows that thread's text.
- A batch may be drained from several threads, but must be freed by one thread after the
  others have stopped calling `next`.
- After `fork()`, a client used in the child builds a fresh engine automatically. A batch
  belongs to the process that submitted it: in a child, `karu_batch_next` rejects it
  with `KARU_ERR_INVALID` and `karu_batch_free` leaves it untouched. Create clients
  before forking or inside each worker, and submit inside the worker.
- ThreadSanitizer reports that mention `Engine`, `karu_batch` or the credential cache are
  bugs in Karu; reproduce with `make test-tsan`.

## 11. Reproducing and reporting

- Inside the repository: `make test-asan` and `make test-tsan`; `python3 tests/http_server.py
  build/tests/karu_http_test` for HTTP behaviour; add a route to `tests/http_server.py`
  to reproduce a server response exactly.
- Protocol and signing problems: reproduce against the emulators
  (`tests/emulators/up.sh`, then `ctest --test-dir build -R karu_emulators`).
- Include `karu_version_string()`, `karu_http_backend()`, the platform, the status and
  the full message.
- Credential leaks, redirect or header forwarding, signature bypasses and memory safety
  issues are security reports: use GitHub's private advisory form for
  `asterisk-labs/karu`, with synthetic credentials and object names.
