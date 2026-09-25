# The read path

Sources: `src/runtime/planner.cpp`, `src/runtime/engine.cpp`, `src/runtime/transport.cpp`,
`src/runtime/http_response.cpp`, `src/runtime/batch.cpp`, `src/client.hpp`,
`benchmarks/coalescing.cpp`, `docs/how-karu-reads.html` (the long-form guide). Timings
and benchmark rows were measured on karu 0.2.3.

## Contents

1. From request to completion
2. Planning and coalescing
3. Scheduling and threads
4. Local reads
5. Remote transfers and connection reuse
6. Response validation
7. Retries, corrections and deadlines
8. Size probes
9. ETags and consistency
10. Performance guidance

## 1. From request to completion

```text
karu_req -> validate -> group by object -> merge neighbours -> Transfer { Part, Part, ... }

  local files:      file queue -> 4 file workers (positional reads)
  S3, GCS, Azure,   credential queue -> 4 credential workers -> HTTP queue
  Source:
  HTTP, Hugging     HTTP queue
  Face:
                    HTTP queue -> KARU_IO_THREADS event loops (curl multi each; together
                                  at most KARU_CONCURRENCY active)

bytes land in each Part's destination -> one completion per karu_req
```

A **transfer** is one local read or one HTTP GET. Every original request becomes a
**part** of exactly one transfer and gets its own completion with its own tag, status and
byte count, so merging never changes what the caller sees.

## 2. Planning and coalescing

`plan_transfers` runs inside `submit`, on the caller's thread:

1. Validation. A read that leaves a bounded window, or whose absolute offset would
   overflow, becomes an immediate `KARU_ERR_RANGE` completion. An `if_match` on a local
   file becomes an immediate `KARU_ERR_UNSUPPORTED` completion.
2. Grouping. Valid reads are stably sorted by backend, canonical path, `if_match` and
   absolute offset (window offset plus request offset). Only neighbours with the same
   backend, canonical path and `if_match` may merge.
3. Merging. A linear sweep extends the current transfer with the next read while every
   guard holds:

| Guard | Option | Default | Holds while |
| --- | --- | --- | --- |
| Gap | `KARU_COALESCE_GAP` | 1 MiB | bytes strictly between the transfer end and the next start are at most the gap |
| Span | `KARU_COALESCE_LIMIT` | 64 MiB | the merged span stays within the limit |
| Parts | `KARU_COALESCE_PARTS` | 1024 | the transfer has fewer parts than the limit |
| Amplification | `KARU_COALESCE_AMPLIFICATION` | 16 | span is at most useful bytes times the ratio |

- Useful bytes are the sum of request lengths, so overlapping and duplicate requests count
  twice. Overlaps and touching ranges merge without consuming the gap.
- A single request larger than the span limit is never split; it becomes one transfer.
- `coalesce_gap = 0` disables merging entirely, even for overlapping local reads.
- Local files use a gap of 0 whatever the setting, so only overlapping or touching local
  ranges merge (they are read once into a scratch buffer and copied into each part).
- Merged remote transfers scatter each network chunk straight into the destinations that
  intersect it. Gap bytes are discarded without staging.

`make benchmark` (`karu_coalescing_benchmark [iterations]`) plans 1000 reads of 64 KiB
without any I/O and prints CSV
(`pattern,policy,requests,transfers,requested_bytes,fetched_bytes,amplification,plan_us`).
Rows from one run:

| Pattern | Policy | Transfers | Fetched / requested |
| --- | --- | --- | --- |
| contiguous (a read every 64 KiB) | defaults | 1 | 1.00 |
| alternating (every 128 KiB) | defaults | 2 | 2.00 |
| alternating | 16 MiB span, amplification 4 | 8 | 1.99 |
| sparse (every 1 MiB) | defaults | 16 | 15.76 |
| sparse | 16 MiB span, amplification 4 | 1000 | 1.00 |
| any | gap 0 | 1000 | 1.00 |

Planning 1000 reads takes well under a millisecond. Pick policies by comparing transfer
count (round trips) with fetched bytes (bandwidth) for the real access pattern.

## 3. Scheduling and threads

- Each client lazily creates one engine on its first submit, fetch or size call:
  `KARU_IO_THREADS` event loops (default 4), each on its own thread with its own curl
  multi handle, pending and retry lists, active transfer map, pool of easy handles and
  connections; 4 credential workers; 4 file workers. The engine lives while the client or
  any of its batches lives.
- Cloud transfers (S3, GCS, Azure, Source) visit a credential worker first. HTTP and
  Hugging Face transfers go straight to the HTTP queue. Anything that can block
  (profiles, helper processes, metadata services, callbacks) stays off the event loops.
- `KARU_CONCURRENCY` is split between the loops (64 over 4 gives 16 each). A loop takes
  transfers from the shared HTTP queue while it has room, so work flows to whichever
  loop frees capacity first, across every batch of the client. The rest wait in the
  queue: a batch can describe millions of reads while active network work stays bounded.
- Retries, region corrections and pooled handles stay with the loop that started the
  transfer, so a retry can reuse that loop's connection. TLS sessions are shared by all
  loops.
- Producers wake idle loops, preferring the lowest index to encourage connection
  reuse. File and credential queues wake only their own workers. Freeing a completed
  batch requires no worker notifications.
- Each batch has its own completion queue. `karu_batch_next` wakes when a completion
  arrives, when the batch is fully drained, or when it is cancelled.
- Cancellation (`karu_batch_free`) removes queued transfers of that batch, asks every
  loop to drop its active and retrying transfers, and waits until the batch's live
  transfer count reaches zero. A credential callback or helper process already running
  for that batch is not interrupted, so the wait can last as long as that call.
- Fork: `karu_client` stores the process id that created its engine. In a child process
  it abandons the inherited mutex and engine (they are never destroyed there) and builds
  new ones.

## 4. Local reads

- Positional reads: `pread` on POSIX, `_lseeki64` plus `_read` on Windows, on a file
  opened per transfer (`O_CLOEXEC`), with UTF-8 paths.
- Four file workers per client, independent of `KARU_CONCURRENCY`.
- End of file inside a request gives `KARU_ERR_RANGE` with the valid prefix in `got`:
  `/data/a.bin: object ended at byte 4096 while reading [4090, +16)`.
- A missing or unreadable file gives `KARU_ERR_IO` with the operating system message.
- No memory mapping, read-ahead or page cache management; the operating system does that.

## 5. Remote transfers and connection reuse

- Easy handles are reset and returned to a pool after each transfer so their
  connections stay open. A curl share object shares TLS sessions.
- Each new connection resolves its host again and shuffles the addresses, so connections
  can use different service endpoints. This adds DNS work when opening connections;
  reused connections do not resolve again.
  Each loop sets `CURLMOPT_MAXCONNECTS` to its share of `KARU_CONCURRENCY`.
- HTTP/1.1 is the default (`KARU_HTTP_VERSION`). HTTP/2 multiplexing is used only when
  requested and negotiated.
- GCS requests send `Accept-Encoding: gzip`, so an object stored with
  `Content-Encoding: gzip` is served as stored and ranges apply to the stored bytes.
  Karu never sets `CURLOPT_ACCEPT_ENCODING`, so libcurl does not decode anything.
- Every transfer: GET with `Range: bytes=first-last`, TCP keepalive, `KARU_CONNECT_TIMEOUT`,
  low-speed abort, a total timeout equal to the remaining request deadline, HTTP and HTTPS
  only (redirects included), at most 10 redirects, `CURLOPT_PATH_AS_IS`, and no
  authentication forwarded to other hosts. The User-Agent is `karu/<version>`.
- Requests are rebuilt for every attempt: URL, headers, signature, region.
- Unwanted bodies up to 256 KiB (error pages, the tail of an over-long response) are
  drained so the connection can be reused; larger ones abort the connection. The first
  4 KiB of an error body are kept for classification, and messages show at most 200
  characters of it with whitespace collapsed.

## 6. Response validation

| Response | Result |
| --- | --- |
| `206` with `Content-Range: bytes F-L/T` where `F` is the requested start, `L` is the requested end or `L + 1 == T`, and the body length equals `L - F + 1` | success; if `L` stops early the object ended inside the request |
| `206` without, or with a malformed or different, `Content-Range` | `KARU_ERR_HTTP`: `Content-Range [0, 15] does not satisfy requested [100, 115]` |
| `200` to a request starting at offset 0 | the first `length` bytes are kept, the rest drained or aborted |
| `200` to a request at offset `N > 0`, `N <= KARU_RANGE_FALLBACK_LIMIT` | the first `N` bytes are skipped; success |
| `200` at offset `N >` the fallback limit | `KARU_ERR_HTTP`: `server ignored Range; refusing to discard 4000 bytes (limit 1024)` |
| `2xx` whose `Content-Encoding` differs from `x-goog-stored-content-encoding`, or that carries `Warning: 214` | `KARU_ERR_HTTP`: the body is not the stored bytes (GCS decompressive transcoding, a rewriting proxy); never retried |
| body shorter than announced | curl `CURLE_PARTIAL_FILE`, retried, then `KARU_ERR_NETWORK`: `transfer closed with 500 bytes remaining to read` |
| `416` | `KARU_ERR_RANGE` (`HTTP 416`) |
| `401`, `403` | `KARU_ERR_AUTH` (after one credential refresh when the provider says the token expired) |
| `404` | `KARU_ERR_NOT_FOUND` |
| `412` | `KARU_ERR_PRECONDITION` |
| `3xx` on a signed request, or any other status | `KARU_ERR_HTTP` with the status and a body summary |

When a transfer ends early because the object ends, parts entirely past the end get
`KARU_ERR_RANGE` with `got == 0`, the part that contains the end gets `KARU_ERR_RANGE`
with its valid prefix, and earlier parts succeed.

## 7. Retries, corrections and deadlines

Transient results are retried while attempts remain:

- libcurl: operation timeout, cannot connect, cannot resolve, empty reply, send or receive
  error, partial body, TLS connect error, HTTP/2 framing or stream error;
- HTTP `408`, `425`, `429`, any `5xx`, and a `400` whose body contains `RequestTimeout`
  (S3's idle connection error).

`COULDNT_RESOLVE_HOST` and `COULDNT_CONNECT` stop after 3 attempts
(`transport::kUnreachableAttempts`), including size and ETag probes. Resolver and
connection timeouts still apply to each attempt.

Interrupted bodies:

- Keep partial bytes only with a single strong caller `if_match` or a strong response
  ETag. The next request sends that tag and asks for the missing bytes.
- Missing, weak or invalid response tags require a full retry. Caller wildcards and
  lists also require a full retry because they can accept more than one version.
- If Karu's saved tag no longer matches, discard the prefix and restart. Exhausted
  attempts return `KARU_ERR_HTTP`. A caller's version mismatch returns
  `KARU_ERR_PRECONDITION` instead.
- If the retry receives a full body (`200`), reread the range from it. The fallback
  limit applies to the original offset.

Delays:

- `Retry-After` (seconds or an HTTP date) is honored up to 60 seconds, plus 0 to 1 second
  of jitter.
- Otherwise the delay before retry `n` is `100 ms << n` (capped at `n = 6`) plus uniform
  jitter of up to the same amount: 200 to 400 ms, then 400 to 800 ms, and so on up to
  6.4 to 12.8 seconds. The waits alone total 19 to 38 seconds for 8 attempts, or 0.6
  to 1.2 seconds for 3. Request time adds to those waits; `Retry-After` can make them
  longer. The request deadline may end the read before all attempts run.

Corrections that do not consume attempts, each at most once per transfer:

- S3 region: a response naming another region (`x-amz-bucket-region` or `<Region>` in the
  error body) re-signs the request for it. The region is remembered for that object for
  the rest of the batch.
- Credentials: an authentication rejection that the provider marks as expired
  invalidates the cached credentials and sends the transfer back to a credential worker.

Deadline:

- `KARU_REQUEST_TIMEOUT` (default 120 seconds) starts when the transfer first needs
  credentials or first starts, and covers every lookup, attempt and wait.
- If the next delay does not fit in what remains, the read fails at once with
  `KARU_TIMEOUT`: `request timeout leaves no time for another attempt`. An expired
  deadline gives `request timeout exceeded`.
- `0` removes the deadline. Low-speed protection still aborts stalled connections.

Everything else is final: `401`, `403`, `404`, `412`, `416`, other `4xx`, validation
failures, blocked redirects, configuration and credential errors, and transient failures
once attempts run out.

## 8. Size probes

- A bounded window answers with its length and no I/O. A local file uses file-system
  metadata.
- A remote object gets `GET` with `Range: bytes=0-0` on the calling thread, with the same
  credentials, signing, retries, region correction and deadline as a read. The size comes
  from the `Content-Range` total; `416` with `bytes */0` means 0; a `200` uses
  `Content-Length`. A `206` without a total (`bytes 0-0/*`) fails with
  `partial response has no valid object size`.
- A ranged GET is used instead of HEAD because it behaves the same across object stores
  and proves read authorization.
- Results are never cached. Cache sizes yourself (a manifest, for example) when they can
  be trusted.

## 9. ETags and consistency

- Each response is checked against its own range, but separate requests can observe
  different versions of an object that is being replaced.
- Give every related read the same `if_match` ETag to make a changed object fail with
  `KARU_ERR_PRECONDITION` (HTTP 412) instead of mixing versions. The value is copied at
  submit, sent as `If-Match`, and covered by the S3, Source and Azure Shared Key
  signatures.
- Karu also compares a single strong `if_match` with the strong `ETag` of the final
  response, and a mismatch fails the same way (`the server ignored If-Match and sent
  ETag ...`). This catches mismatches on CDNs that ignore the condition. If the
  response has no strong ETag, enforcement depends on the server. `*` and lists are
  left to the server and do not identify one version.
- Reads with different `if_match` values never share a transfer.
- `karu_client_stat` (C++ `Client::stat`) returns the size and the strong ETag in one
  ranged `GET`; pass that ETag as `if_match` to pin every later read to the version.
  Use the ETag from the final object response; a hub's `X-Linked-Etag` may identify
  something else. Local files reject ETags.
- `stat` is optional and does not validate format headers. The format reader must
  pair external metadata with the correct object version. Resume checks protect one
  transfer; they do not create a snapshot across a batch.

## 10. Performance guidance

- Keep one client per process (or per configuration) and let it live. A new client pays
  for new threads, TCP and TLS handshakes and credential discovery.
- Submit whole batches (every range of a sample, or of a minibatch) instead of one read at
  a time. Concurrency and coalescing only work on what has been submitted.
- Tune `KARU_CONCURRENCY` across all data-loader workers, not just one client.
  Watch for `429` and `503` from the service when raising it: the
  batch counters (`karu_batch_get_stats`) report `throttled` responses, `retries`,
  `new_connections` and `received_bytes` against `requested_bytes`.
- Drain completions as they arrive and decode in parallel; `karu_batch_next` may be
  called from several threads.
- Measure against the real storage service. A loopback server has no latency, which hides
  the effect of concurrency and coalescing, and cold connections distort the first batch.
- Keep format planning above Karu: rumi plans frame ranges (`plan_ranges`), submits them
  with the frame as `tag`, and decodes each completion as it lands.
