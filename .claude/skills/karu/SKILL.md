---
name: karu
description: >-
  Use Karu or work on its codebase: read exact byte ranges from local files, HTTP,
  S3, GCS, Azure, Hugging Face and Source Cooperative through the C ABI or the C++23
  facade; write URI, VSI and /vsisubfile/ paths; submit, coalesce and drain batches;
  set options, path rules and credentials; explain karu_status failures; or edit
  Karu's engine, backends, tests, emulators and CMake packaging. Do not use for GDAL
  itself or for decoding file formats.
---

# Karu

Karu reads exact byte ranges. Every read names an object, an offset, a length, a
destination buffer and a caller tag, so reads carry no cursor and can be reordered,
merged and retried. Karu is read only and format free: it resolves addresses, reports
sizes and moves bytes. It never lists, writes, parses formats or caches object data.
A format layer such as rumi decides which ranges matter and decodes what comes back.

This skill describes **karu 0.2.3** (C API version 1). Check `karu_version_string()` or
the `VERSION` file. If they differ, trust the source, `CHANGELOG.md` and
`CONFIGURATION.md` over this file.

## Mental model

- **Config builder** (`karu_config`, `karu::Config`): mutable. `karu_config_create`
  snapshots the environment once; `karu_config_create_empty` reads nothing.
- **Client** (`karu_client`, `karu::Client`): an immutable copy of the configuration that
  lazily owns one engine: `KARU_IO_THREADS` libcurl event loops (4 by default), 4
  credential workers, 4 file workers, pooled connections and a credential cache. Share it
  across threads.
- **Locator** (`karu_locator`, `karu::Object`): a parsed address with a canonical path
  and an optional window. Parsing does no I/O and loads no credentials.
- **Batch**: submit many requests at once. The planner groups them by object, merges
  neighbours under four guards, runs at most `KARU_CONCURRENCY` remote transfers, and
  scatters bytes back so that every original request gets its own completion
  (tag, status, bytes read) in completion order.

## Canonical workflow

```cpp
#include <karu/karu.hpp>

#include <array>
#include <cstddef>

auto config = karu::Config::from_environment().value();
config.set("KARU_CONCURRENCY", "256").value();
config.set_path("s3://public-bucket/", "AWS_NO_SIGN_REQUEST", "YES").value();
auto client = karu::Client::create(config).value();          // reuse for the whole process
auto object = karu::Object::parse("s3://public-bucket/scene.tif").value();

std::array<std::byte, 4096> a{}, b{};
const std::array<karu::Read, 2> reads{{
    {.object = &object, .offset = 0, .destination = a, .tag = &a},
    {.object = &object, .offset = 1 << 20, .destination = b, .tag = &b},
}};
auto batch = client.submit(reads).value();
for (auto event = batch.next().value(); event.state != karu::BatchState::end;
     event = batch.next().value()) {
    if (event.status != KARU_OK) { /* event.message explains this read */ }
    // event.tag says which buffer is ready; only event.bytes_read bytes are defined
}
```

`.value()` throws `std::bad_expected_access<karu::Error>`; check results explicitly in
library code. The C ABI has the same shape (`karu_client_submit`, `karu_batch_next`).

## Paths

| Backend | URI | VSI path |
| --- | --- | --- |
| Local file | `/data/a.bin`, `file:///data/a.bin` | |
| HTTP(S) | `https://host/object` | `/vsicurl/https://host/object` |
| S3 and compatible | `s3://bucket/key` | `/vsis3/bucket/key` |
| Google Cloud Storage | `gs://bucket/key` | `/vsigs/bucket/key` |
| Azure Blob / Data Lake | `az://container/blob`, `abfs://container/path` | `/vsiaz/...`, `/vsiadls/...` |
| Hugging Face | `hf://[datasets/\|spaces/]owner/name[@rev]/path` | `/vsihf/...` |
| Source Cooperative | `source://account/product/key` | `/vsisource/...` |

Byte windows: `/vsisubfile/OFFSET_LENGTH,<path>` and a `#bytes=FIRST-LAST` fragment
rebase offsets to zero and reject reads that leave the window.

## Choosing a call

| Need | Use |
| --- | --- |
| One blocking read | `Client::read` or `read_into`; C `karu_client_fetch` with one request |
| Many reads, one pass or fail | `Client::fetch`, `karu_client_fetch` (first failure only) |
| Many reads, results as they finish | `Client::submit` plus `Batch::next`; C `karu_client_submit` plus `karu_batch_next` |
| Different coalescing for one batch | `SubmitOptions`, `karu_client_submit_with`, `karu_client_fetch_with` |
| Karu allocates the buffers | C only: `karu_req.buffer = NULL`, then `karu_free(done.buffer)` |
| Fail if the object changed | the same `if_match` ETag on every read gives `KARU_ERR_PRECONDITION` |
| Visible size | `karu_client_size` (a one-byte GET, never cached; bounded windows need no I/O) |
| Size and ETag to pin reads | `karu_client_stat` / `Client::stat` (the same GET; the ETag goes to `if_match`) |
| Tune concurrency and coalescing | `karu_batch_get_stats` / `Batch::stats`: retries, 429/503, new connections, received vs requested bytes |
| Credentials from your own SDK | `karu_config_set_credentials_provider` |

## Invariants and pitfalls

- Test `status != KARU_OK`, never `status < 0`. `KARU_TIMEOUT` is positive and is also a
  per-read failure when `KARU_REQUEST_TIMEOUT` expires.
- `karu_batch_next` returning `KARU_OK` means one completion is ready; its own `status`
  says whether that read worked. `KARU_END` and `KARU_TIMEOUT` leave the output untouched.
- Only the first `got` (`bytes_read`) bytes are defined. `KARU_ERR_RANGE` can deliver a
  valid prefix when the object ends inside the request. After a failed `fetch`, every
  destination is unspecified.
- Destination buffers must outlive the batch. Freeing a batch cancels its work and blocks
  until workers let go of the buffers; never free it while another thread is in `next`.
  The config, client and locators may be freed right after `submit`.
- `karu_last_error()` is thread local, capped at 511 bytes and cleared by the next
  fallible Karu call on that thread. Copy it at once.
- The environment is read by `karu_config_create` only. Unknown option names fail at
  `set`; bad values fail later, at `karu_client_create`, with `KARU_ERR_CONFIG`. A
  client never sees later builder changes: create a new client.
- `KARU_*` options, including `KARU_HTTP_*`, are client wide. Path rules match whole
  segments and the longest prefix wins, per option.
- S3, GCS and Azure sign by default and fail with `KARU_ERR_CREDENTIALS` when nothing is
  found. Public data needs `AWS_NO_SIGN_REQUEST=YES` (or the GCS and Azure
  equivalents). Source Cooperative is anonymous unless configured, and Hugging Face
  sends a token only when it finds one.
- Setting `KARU_HTTP_HEADERS` restricts redirects to the same origin, which blocks
  Hugging Face files served from a CDN.
- Requests need a finite, nonzero length. `KARU_TO_END` is only a window length.
- Reuse one client. Each new client starts new threads, cold connections and an empty
  credential cache. After `fork()` a client rebuilds its engine on first use in the
  child. Inherited batches return `KARU_ERR_INVALID`; freeing them is a no-op.
- Submitted cloud reads resolve credentials on credential workers. A synchronous
  `karu_client_size` or `karu_client_stat` call performs its credential lookup, callback and
  any `credential_process` on the calling thread.
- Keep format knowledge out of Karu. Plan ranges above it, submit them in batches, and
  decode completions as they arrive.

## Reference map

Read only the reference relevant to the current task. Each one names its source files
in the repository and is scoped to karu 0.2.3.

| Task | Read |
| --- | --- |
| C ABI: handles, ownership, statuses, batches, callbacks, threads, fork, bindings, CMake | [references/c-api.md](references/c-api.md) |
| C++23 facade: `Config`, `Object`, `Client`, `Read`, `SubmitOptions`, `Batch` | [references/cpp-api.md](references/cpp-api.md) |
| URI and VSI grammar, canonical paths, keys, windows, Hugging Face revisions | [references/paths.md](references/paths.md) |
| Option precedence, aliases, validation, path rules, tuning, HTTP options | [references/configuration.md](references/configuration.md) |
| Signed or anonymous, discovery chains per provider, custom providers, cache and refresh | [references/credentials.md](references/credentials.md) |
| Planner, scheduling, response checks, retries, deadlines, size probes, ETags, performance | [references/read-path.md](references/read-path.md) |
| Repository layout, build, test suites, fixtures, emulators, adding options or backends, CI, releases | [references/contributing.md](references/contributing.md) |
| Status and message lookup, credential, TLS, redirect, timeout and fork problems | [references/debugging.md](references/debugging.md) |
