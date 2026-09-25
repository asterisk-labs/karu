# Configuration

Sources: `src/config.cpp`, `src/config_options.hpp`, `src/request_builder.cpp`,
`src/backends/credentials.cpp` (`add_configured_headers`). `CONFIGURATION.md` at the
repository root is the complete option table and is installed with the package; this
file explains how the options combine. Messages were captured from karu 0.2.3.

## Contents

1. Layers and precedence
2. Names, aliases and validation
3. Path rules
4. Runtime options and tuning
5. HTTP options
6. Signed or anonymous by default
7. Changing configuration
8. Hermetic configurations

## 1. Layers and precedence

For an option `N` read for canonical path `P`, the first layer that defines `N` wins:

1. the path rule with the longest prefix that matches `P` **and defines `N`**;
2. an explicit value from `karu_config_set_option` / `Config::set`;
3. the environment snapshot taken by `karu_config_create` / `Config::from_environment`;
4. the built-in default.

- A path rule beats an explicit global value.
- Rules are consulted per option: a deeper rule that does not define `AWS_REGION` does
  not hide a shallower rule that does.
- The environment is read once, when the builder is created. Later `setenv` calls are
  invisible to that builder and to every client made from it.
- `karu_config_create_empty` has no environment layer.

## 2. Names, aliases and validation

- Names are case insensitive. `KARU_THREADS` and other unknown names fail immediately
  with `KARU_ERR_CONFIG`: `unknown Karu option 'KARU_THREADS'`.
- Empty environment variables are ignored. An empty value set through the API is kept,
  which is how `KARU_HTTP_USER_AGENT` is disabled.
- Aliases fold into one canonical name, so an alias can never bypass a more specific
  path rule written under the canonical name:

| Canonical | Aliases, in environment precedence order |
| --- | --- |
| `AWS_S3_ENDPOINT` | `AWS_ENDPOINT_URL_S3`, `AWS_ENDPOINT_URL` |
| `AWS_REGION` | `AWS_DEFAULT_REGION` |
| `AWS_PROFILE` | `AWS_DEFAULT_PROFILE` |
| `KARU_MAX_ATTEMPTS` | `KARU_MAX_RETRIES` |
| `HF_TOKEN` | `HUGGING_FACE_HUB_TOKEN` |
| `KARU_HTTP_CA_BUNDLE` | `CURL_CA_BUNDLE`, `SSL_CERT_FILE` |
| `SOURCE_ENDPOINT` | `SOURCE_PROXY_URL` |

  In the environment the canonical spelling wins, then the aliases in table order.
  Through the API every spelling writes the canonical name, so the last call wins.

- Values are validated when a client is created (`karu_client_create`,
  `karu_client_matches_config`), in the environment snapshot, the explicit layer and
  every path rule. A bad value inherited from the environment therefore makes client
  creation fail even though nothing in the program set it.

| Option | Accepted | Message on failure |
| --- | --- | --- |
| `AWS_NO_SIGN_REQUEST`, `AWS_HTTPS`, `AWS_VIRTUAL_HOSTING`, `AWS_EC2_METADATA_DISABLED`, `GCS_NO_SIGN_REQUEST`, `GCS_METADATA_DISABLED`, `AZURE_NO_SIGN_REQUEST`, `SOURCE_NO_SIGN_REQUEST` | `YES/NO`, `TRUE/FALSE`, `ON/OFF`, `1/0`, any case | `AWS_NO_SIGN_REQUEST must be YES/NO, TRUE/FALSE, ON/OFF, or 1/0` |
| `AWS_METADATA_SERVICE_TIMEOUT` | 1 to 60 | `AWS_METADATA_SERVICE_TIMEOUT must be an integer in [1, 60]` |
| `AWS_REQUEST_PAYER` | `requester` | `AWS_REQUEST_PAYER must be 'requester'` |
| `KARU_HTTP_VERSION` | `1.1`, `AUTO`, `2`, `2TLS`, `2PRIOR_KNOWLEDGE` | `KARU_HTTP_VERSION must be AUTO, 1.1, 2TLS, 2, or 2PRIOR_KNOWLEDGE` |
| numeric `KARU_*` | see section 4 | `KARU_CONCURRENCY must be an integer in [1, 4096]` |

- A value inside a path rule is reported with the rule prefix in front.
- Endpoints (`AWS_S3_ENDPOINT`, `GCS_ENDPOINT`, `AZURE_STORAGE_ENDPOINT`, `HF_ENDPOINT`,
  `SOURCE_ENDPOINT`) are checked when a request is built, so a bad endpoint fails each
  read with `KARU_ERR_CONFIG`. They must be HTTP or HTTPS, without user information,
  query or fragment. A host without a scheme gets `https://` (for S3, `http://` when
  `AWS_HTTPS=NO`).

## 3. Path rules

```cpp
auto config = karu::Config::from_environment().value();
config.set("AWS_REGION", "us-west-2").value();
config.set_path("s3://public-data/", "AWS_NO_SIGN_REQUEST", "YES").value();
config.set_path("s3://lab/", "AWS_PROFILE", "research").value();
config.set_path("s3://minio-bucket/", "AWS_S3_ENDPOINT", "http://127.0.0.1:9000").value();
config.set_path("s3://minio-bucket/", "AWS_REGION", "us-east-1").value();
```

- The prefix is normalized like a URI (`s3://` becomes `/vsis3/`, `hf://` becomes
  `/vsihf/`, and so on). HTTP URLs and local paths are used as written.
- A prefix matches itself and its descendants at segment boundaries:
  `/vsis3/team/private` matches `/vsis3/team/private/file` but not
  `/vsis3/team/private-copy/file`. A prefix ending in `/` matches everything below it.
- `KARU_*` options are client wide and cannot be path rules:
  `KARU_CONCURRENCY is client-wide and cannot be path-specific`. This includes every
  `KARU_HTTP_*` option; use a second client for a different transport policy.
- Setting a value to `NULL` (`unset_path`) removes it from that rule.
- Path rules also shape credential scopes: see `credentials.md`.

## 4. Runtime options and tuning

| Option | Default | Range | Effect |
| --- | --- | --- | --- |
| `KARU_CONCURRENCY` | 64 | 1 to 4096 | active remote transfers per client, across all its batches; also libcurl's connection cache size |
| `KARU_IO_THREADS` | 4 | 1 to 64 | libcurl event loops, each on its own thread; `KARU_CONCURRENCY` is split between them and each keeps its own connections |
| `KARU_COALESCE_GAP` | 1048576 | 0 and up | largest gap merged between remote ranges; 0 disables merging |
| `KARU_COALESCE_LIMIT` | 67108864 | 1 and up | largest merged span |
| `KARU_COALESCE_PARTS` | 1024 | 1 to 1048576 | most requests served by one transfer |
| `KARU_COALESCE_AMPLIFICATION` | 16 | 1 to 1048576 | most transferred bytes per requested byte |
| `KARU_RANGE_FALLBACK_LIMIT` | 8388608 | 0 and up | largest prefix discarded when a server ignores `Range`; 0 refuses every nonzero offset |
| `KARU_MAX_ATTEMPTS` | 8 | 1 to 16 | total attempts for transient failures; at most 3 when the host does not resolve or refuses the connection |
| `KARU_REQUEST_TIMEOUT` | 120 | 0 to 86400 s | whole logical read: credentials, attempts and waits; also kills `credential_process`; 0 disables |
| `KARU_CONNECT_TIMEOUT` | 30 | 1 to 3600 s | connection setup |
| `KARU_LOW_SPEED_TIME` | 60 | 0 to 3600 s | abort when slower than the limit for this long |
| `KARU_LOW_SPEED_LIMIT` | 1024 | 0 and up, bytes/s | low-speed threshold |

Tuning notes:

- `KARU_CONCURRENCY` limits active remote transfers per client. Increase it when
  requests spend most of their time waiting on the service; reduce it if HTTP 429/503
  responses rise. `karu_batch_get_stats` reports those responses as `throttled`.
- `KARU_IO_THREADS` runs TLS, signing and copies on separate cores. Each loop has
  its own connections. Compare 1 and 4 on the actual workload, counting all clients
  and leaving CPU time for decoding.
- Local files use 4 file workers per client; `KARU_CONCURRENCY` does not apply.
- Coalescing trades fewer requests for extra bytes. Lower
  `KARU_COALESCE_AMPLIFICATION` or `KARU_COALESCE_GAP` if gaps dominate the data fetched.
  Compare `received_bytes` with `requested_bytes`, accounting for retries and responses
  that ignore ranges. `make benchmark` measures the planner, not network throughput.
- Coalescing can be changed per batch with `karu_client_submit_with` or C++
  `SubmitOptions`. Other runtime options are fixed per client.
- More attempts can recover transient failures but delay the final error. Reduce
  `KARU_MAX_ATTEMPTS` or `KARU_REQUEST_TIMEOUT` when latency matters more.
  DNS and connection failures stop after 3 attempts. See `read-path.md`, section 7.

## 5. HTTP options

| Option | Default | Notes |
| --- | --- | --- |
| `KARU_HTTP_HEADERS` | none | newline-separated `Name: value` lines added to every request |
| `KARU_HTTP_VERSION` | `1.1` | `AUTO` negotiates; `2`/`2TLS` asks for HTTP/2 over TLS; `2PRIOR_KNOWLEDGE` for cleartext HTTP/2 |
| `KARU_HTTP_CA_BUNDLE` | system bundle | aliases `CURL_CA_BUNDLE`, `SSL_CERT_FILE` |
| `KARU_HTTP_CA_PATH` | none | directory of certificates |
| `KARU_HTTP_PROXY` | none | libcurl's `http_proxy`, `https_proxy` and `no_proxy` variables also apply |
| `KARU_HTTP_PROXY_CREDENTIALS` | none | `user:password` |
| `KARU_HTTP_USER_AGENT` | `karu/<version>` | an empty value set through the API sends no User-Agent |

- `KARU_HTTP_HEADERS` may not set `Host`, `Range` or a header Karu already generated
  for that request. For cloud and Hugging Face requests it may also not set
  `Authorization`, `Date`, `If-Match`, `x-amz-*`, `x-goog-*` or `x-ms-*`. Violations fail
  each read with `KARU_ERR_CONFIG`:
  `KARU_HTTP_HEADERS cannot override Karu-managed header 'Range'`.
- Whenever `KARU_HTTP_HEADERS` is set, redirects are followed only within the original
  scheme, host and port, so the headers never reach another origin. This also blocks
  Hugging Face downloads that redirect to a CDN.
- When neither CA option is set, Karu passes the first bundle it finds at
  configuration time: on Linux `/etc/ssl/certs/ca-certificates.crt`,
  `/etc/pki/tls/certs/ca-bundle.crt`, `/etc/ssl/ca-bundle.pem`, `/etc/pki/tls/cacert.pem`,
  `/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem`, `/etc/ssl/cert.pem`; on macOS
  `/etc/ssl/cert.pem`. Elsewhere, and for empty configurations, libcurl's default applies.
- Credential endpoints (STS, OAuth, metadata services) use the same CA, proxy,
  User-Agent and HTTP version settings.

## 6. Signed or anonymous by default

| Backend | Without configuration | Anonymous switch |
| --- | --- | --- |
| S3 | signed; fails with `KARU_ERR_CREDENTIALS` when no credentials are found | `AWS_NO_SIGN_REQUEST=YES` |
| GCS | signed | `GCS_NO_SIGN_REQUEST=YES` |
| Azure | signed; even anonymous reads need an account name or endpoint (option or connection string) | `AZURE_NO_SIGN_REQUEST=YES` |
| Source Cooperative | anonymous; signed once any `SOURCE_*` credential option or a Source callback exists | `SOURCE_NO_SIGN_REQUEST=YES` or `NO` |
| Hugging Face | token when one is found, otherwise anonymous | none |
| HTTP | no authentication | use `KARU_HTTP_HEADERS` |

Use a path rule for public buckets so private buckets in the same client keep signing:
`set_path("s3://public-bucket/", "AWS_NO_SIGN_REQUEST", "YES")`.

## 7. Changing configuration

- A client freezes its configuration. Changing the builder afterwards has no effect on
  it; create a new client (and let the old one finish its batches).
- `karu_client_matches_config` (`Client::matches`) compares a builder with a client
  without I/O, which lets a binding reuse a cached client.
- Creating a client is cheap, but its engine starts cold: new threads, no pooled
  connections, no TLS sessions and an empty credential cache.

## 8. Hermetic configurations

`karu_config_create_empty()` (`Config::empty()`) reads no environment variable, home
directory or CA bundle, and disables implicit discovery: no `~/.aws` files, no gcloud
ADC file, no Hugging Face token cache, no instance metadata. Explicit settings opt back
in one source at a time:

| Source | Opt in with |
| --- | --- |
| AWS profile files | `AWS_CONFIG_FILE`, `AWS_SHARED_CREDENTIALS_FILE` (and `AWS_PROFILE`) |
| AWS IMDS | `AWS_EC2_METADATA_DISABLED=NO` |
| GCS ADC | `GOOGLE_APPLICATION_CREDENTIALS` or `CLOUDSDK_CONFIG` |
| GCS metadata | `GCS_METADATA_ENDPOINT` or `GCS_METADATA_DISABLED=NO` |
| Azure managed identity | `IMDS_ENDPOINT` or `IDENTITY_ENDPOINT` |
| Hugging Face token file | `HF_TOKEN_PATH` or `HF_HOME` |
| Source profile files | `SOURCE_CONFIG_FILE`, `SOURCE_SHARED_CREDENTIALS_FILE` |

The test suites build configurations this way, apart from the tests that exercise the
environment layer on purpose, which is why they never touch the developer's real
credentials.
