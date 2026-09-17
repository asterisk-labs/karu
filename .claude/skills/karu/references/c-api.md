# C API

The stable boundary is `include/karu/karu.h`; its implementation is `src/karu.cpp`.
The examples below compile with `-std=c11 -Wall -Wextra -Wpedantic -Werror` against an
installed karu 0.2.3 and ran on local files, `tests/http_server.py` and `hf://`.

## Contents

1. Versions, linking and CMake
2. Handles and ownership
3. Status codes and error text
4. Configuration and clients
5. Locators and sizes
6. Requests, batches and completions
7. Example: submit and drain
8. Blocking fetch and per-batch options
9. Credential callbacks
10. Threads and fork
11. Writing bindings

## 1. Versions, linking and CMake

| Call | Returns |
| --- | --- |
| `karu_api_version()` | `KARU_API_VERSION`, currently `1` |
| `karu_version_string()` | the exact release string from the `VERSION` file |
| `karu_http_backend()` | libcurl's `curl_version()`, for example `libcurl/8.7.1 SecureTransport (LibreSSL/3.3.6) zlib/1.2.12 nghttp2/1.68.0` |

- Karu builds only as a static C++23 library (`libkaru.a`, `karu.lib`) with hidden
  visibility. `KARU_API` expands to nothing. A C program must be linked by a C++ linker.
- Link dependencies propagate through the CMake target: `CURL::libcurl` 7.83.0 or newer,
  `OpenSSL::Crypto` 3, `Threads::Threads`.
- The ABI has opaque handles, plain structs and numeric statuses; no C++ type or
  exception crosses it. `karu_req`, `karu_done` and `karu_credentials` have no size
  field, so changing them breaks callers. `karu_submit_options` grows through
  `struct_size`.

Installed package (`cmake --install` writes `lib/cmake/karu`, version compatibility
`SameMajorVersion`):

```cmake
find_package(karu 0.2 CONFIG REQUIRED)
add_executable(reader reader.c)
set_target_properties(reader PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(reader PRIVATE karu::karu)
```

Generic vendored source:

```cmake
add_subdirectory(extern/karu ${CMAKE_CURRENT_BINARY_DIR}/karu EXCLUDE_FROM_ALL)
target_link_libraries(app PRIVATE karu::karu)
```

The build tree also exposes `karu::internal` for rumi's private vendored integration;
that alias is not exported by the installed package.

On macOS, pass `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)` if CMake cannot find
OpenSSL. On Windows, CI uses `vcpkg install curl:x64-windows openssl:x64-windows` and the
vcpkg toolchain file.

## 2. Handles and ownership

| Handle | Created by | Released by | Sharing |
| --- | --- | --- | --- |
| `karu_config*` | `karu_config_create`, `karu_config_create_empty` | `karu_config_free` | mutable; synchronize externally |
| `karu_client*` | `karu_client_create(config, &client)` | `karu_client_free` | share across threads |
| `karu_locator*` | `karu_resolve(uri, &locator)` | `karu_locator_free` | immutable |
| `karu_batch*` | `karu_client_submit`, `karu_client_submit_with` | `karu_batch_free` | `karu_batch_next` from any number of threads |
| Karu buffer | a request with `buffer = NULL` | `karu_free(done.buffer)` | yours once `karu_batch_next` returns it |

- Every free function accepts `NULL`. Creating functions reset `*out` to `NULL` only
  after their pointer arguments pass the null check, so initialize handles to `NULL`
  yourself.
- `karu_client_create` copies the configuration; free or reuse the builder afterwards.
- Submission copies requests, locators and ETags, and a batch keeps its engine alive.
  The config, client and locators may therefore be freed while a batch still drains.
  Only destination buffers must outlive the batch.
- `karu_batch_free` releases Karu-allocated buffers that were never drained. Drained ones
  belong to the caller whatever the completion status.

## 3. Status codes and error text

| Value | Name | `karu_status_string` | Produced by |
| --- | --- | --- | --- |
| 0 | `KARU_OK` | `ok` | |
| 1 | `KARU_END` | `batch drained` | `karu_batch_next` |
| 2 | `KARU_TIMEOUT` | `timed out` | a `karu_batch_next` wait that expired; a read whose `KARU_REQUEST_TIMEOUT` expired; a killed `credential_process` |
| -1 | `KARU_ERR_URI` | `malformed URI` | `karu_resolve` |
| -2 | `KARU_ERR_UNSUPPORTED` | `unsupported virtual filesystem` | `/vsizip/` and other handlers; an ETag on a local file |
| -3 | `KARU_ERR_INVALID` | `invalid argument` | null arguments, empty or unbounded requests, bad submit options, internal C++ exceptions |
| -4 | `KARU_ERR_NOMEM` | `out of memory` | Karu-owned buffers, staging, handles |
| -5 | `KARU_ERR_IO` | `file error` | open or read of a local file |
| -6 | `KARU_ERR_NETWORK` | `network error` | connection, DNS, TLS, truncated bodies, refused redirect protocols |
| -7 | `KARU_ERR_HTTP` | `http error` | other HTTP statuses, bad `Content-Range`, ignored `Range` over the limit, blocked or unfollowed redirects |
| -8 | `KARU_ERR_RANGE` | `range past end of object` | HTTP 416, an object ending inside the request, a read leaving its window |
| -9 | `KARU_ERR_AUTH` | `not authorized` | HTTP 401 and 403 |
| -10 | `KARU_ERR_CANCELLED` | `cancelled` | reserved in this release; cancelled work is discarded |
| -11 | `KARU_ERR_CONFIG` | `invalid configuration` | unknown options, invalid values, bad endpoints |
| -12 | `KARU_ERR_CREDENTIALS` | `credentials unavailable` | missing or failed credentials, callback failures |
| -13 | `KARU_ERR_NOT_FOUND` | `object not found` | HTTP 404 |
| -14 | `KARU_ERR_PRECONDITION` | `precondition failed` | HTTP 412 for `if_match` |

- Compare with `!= KARU_OK`. `KARU_TIMEOUT` is positive and can be a failure.
- `karu_last_error()` returns thread-local text, truncated to 511 bytes. Every function
  that returns a `karu_status` clears it on entry, so copy it before the next such call
  on the same thread. `karu_clear_error()` clears it explicitly.
- `karu_batch_next` stores the message of the failed completion it returns, so read
  `karu_last_error()` right after it. `karu_client_fetch` stores the message of the first
  failure.
- Argument errors start with the function name. Read failures start with the object: a
  local path, a canonical VSI path, or an HTTP URL whose user information, query and
  fragment are replaced by `<redacted>`.

## 4. Configuration and clients

| Call | Behaviour |
| --- | --- |
| `karu_config_create(&config)` | snapshots supported environment variables (empty values are ignored), home and cache directories and, on Linux and macOS, the system CA bundle; enables default credential discovery |
| `karu_config_create_empty(&config)` | reads nothing and discovers nothing; explicit options still work |
| `karu_config_set_option(config, name, value)` | case-insensitive name, aliases folded; `NULL` removes; unknown names fail here with `KARU_ERR_CONFIG` |
| `karu_config_set_path_option(config, prefix, name, value)` | option for one path and its descendants; `KARU_*` names are rejected (see `configuration.md`) |
| `karu_config_set_credentials_provider(config, kind, provider, user_data, release)` | installs one callback per kind; clear with `provider`, `user_data` and `release` all `NULL` |
| `karu_client_create(config, &client)` | validates every value and freezes a snapshot; invalid values fail here with `KARU_ERR_CONFIG` |
| `karu_client_matches_config(client, config, &match)` | 1 when `config` would freeze to the client's snapshot; no I/O; callbacks compare by registration, so installing the same function again does not match |
| `karu_client_concurrency`, `karu_client_coalesce_gap`, `karu_client_max_attempts` | frozen values; 0 for a `NULL` client |

The engine (threads, curl handles, credential cache) starts on the first submit, fetch
or size call, not at client creation.

## 5. Locators and sizes

- `karu_resolve(uri, &locator)` parses only. It returns `KARU_ERR_URI` or
  `KARU_ERR_UNSUPPORTED` with a message; see `paths.md` for the grammar.
- `karu_locator_uri` is the canonical identity used for configuration, grouping and
  credential scope. `karu_locator_is_remote` is nonzero for everything but local files.
  `karu_locator_window_offset` and `karu_locator_window_length` describe a window;
  the length is `KARU_TO_END` when unbounded.
- `karu_client_size(client, locator, &size)` reports the visible size:
  - bounded window: the window length, with no I/O;
  - local file: file size minus the window offset;
  - remote object: a blocking `GET` of byte 0 on the calling thread, with credentials,
    retries and region correction, reading the total from `Content-Range`. The result is
    never cached, and an empty object reports 0.

## 6. Requests, batches and completions

```c
typedef struct {
    const karu_locator* locator;
    uint64_t offset;        /* relative to the locator window */
    uint64_t length;        /* nonzero and finite */
    void* buffer;           /* at least length bytes, or NULL to let Karu allocate */
    void* tag;              /* returned untouched */
    const char* if_match;   /* optional ETag, copied at submit */
} karu_req;

typedef struct {
    void* tag;
    karu_status status;
    uint64_t got;           /* defined prefix of buffer */
    void* buffer;           /* the request buffer, or the one Karu allocated */
} karu_done;
```

Submission rules:

- `locator == NULL`, `length == 0`, `length == KARU_TO_END` or an `if_match` containing
  CR or LF fails the whole submit with `KARU_ERR_INVALID`, for example
  `karu_client_submit: request 0 is empty`.
- Problems that depend on the locator do not fail submit. A read leaving its window
  arrives as a `KARU_ERR_RANGE` completion and an ETag on a local file as
  `KARU_ERR_UNSUPPORTED`, while valid reads in the same batch continue.
- `count == 0` returns a batch that reports `KARU_END` at once.
- Overlapping and duplicate requests are valid; each destination receives its own copy.

Draining:

- `karu_batch_next(batch, &done, timeout_ms)`: `-1` waits, `0` polls, a positive value
  waits that many milliseconds.
- `KARU_OK`: one completion is in `done`. `KARU_END`: every request has been returned
  (or the batch was cancelled). `KARU_TIMEOUT`: nothing ready yet; `done` is unchanged.
- Completions arrive in completion order, not submission order. Match them by `tag`.
- Several threads may call `karu_batch_next` on one batch. Each completion goes to
  exactly one caller and every caller eventually receives `KARU_END`.
- `done.got` equals the requested length on success. It is normally 0 on failure, but
  `KARU_ERR_RANGE` reports the valid prefix when the object ends inside the request.
  Bytes past `got` are unspecified.
- `karu_batch_free` marks the batch cancelled, removes queued work, and waits until no
  worker or network transfer still references its buffers. Do not call it concurrently
  with `karu_batch_next` on the same batch.

## 7. Example: submit and drain

```c
// Reads four 16-byte ranges spread across one object and drains them as they finish.
#include <karu/karu.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

enum { COUNT = 4, LENGTH = 16 };

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s PATH_OR_URI\n", argv[0]);
        return 2;
    }

    karu_config* config = NULL;
    karu_client* client = NULL;
    karu_locator* object = NULL;
    karu_batch* batch = NULL;
    unsigned char buffers[COUNT][LENGTH];
    karu_req requests[COUNT];
    int failed = 1;

    // Snapshot the environment, override one option, and freeze it into a client.
    if (karu_config_create(&config) != KARU_OK ||
        karu_config_set_option(config, "KARU_CONCURRENCY", "128") != KARU_OK ||
        karu_client_create(config, &client) != KARU_OK) {
        fprintf(stderr, "setup: %s\n", karu_last_error());
        goto cleanup;
    }
    if (karu_resolve(argv[1], &object) != KARU_OK) {
        fprintf(stderr, "resolve: %s\n", karu_last_error());
        goto cleanup;
    }

    uint64_t size = 0;
    if (karu_client_size(client, object, &size) != KARU_OK) {
        fprintf(stderr, "size: %s\n", karu_last_error());
        goto cleanup;
    }
    if (size < LENGTH) {
        fprintf(stderr, "object has only %" PRIu64 " bytes\n", size);
        goto cleanup;
    }

    for (size_t index = 0; index < COUNT; ++index) {
        requests[index] = (karu_req){
            .locator = object,
            .offset = (size - LENGTH) / (COUNT - 1) * index,
            .length = LENGTH,
            .buffer = buffers[index], // NULL asks Karu to allocate; release it with karu_free
            .tag = &requests[index],
            .if_match = NULL,
        };
    }
    if (karu_client_submit(client, requests, COUNT, &batch) != KARU_OK) {
        fprintf(stderr, "submit: %s\n", karu_last_error());
        goto cleanup;
    }

    failed = 0;
    karu_done done;
    karu_status step;
    while ((step = karu_batch_next(batch, &done, -1)) == KARU_OK) {
        const karu_req* request = done.tag;
        if (done.status != KARU_OK) {
            // The message belongs to this completion and stays valid until the
            // next Karu call on this thread.
            fprintf(stderr, "offset %" PRIu64 ": %s: %s\n", request->offset,
                    karu_status_string(done.status), karu_last_error());
            failed = 1;
            continue;
        }
        const unsigned char* bytes = done.buffer;
        printf("offset %" PRIu64 ": %" PRIu64 " bytes, first 0x%02x\n", request->offset, done.got,
               bytes[0]);
    }
    if (step != KARU_END) {
        fprintf(stderr, "next: %s\n", karu_last_error());
        failed = 1;
    }

cleanup:
    karu_batch_free(batch); // cancels unfinished work before the buffers go away
    karu_locator_free(object);
    karu_client_free(client);
    karu_config_free(config);
    return failed;
}
```

Output for a 1 MiB local file whose byte `i` is `(i * 31 + 7) & 0xFF` (the order
changes from run to run):

```text
offset 699040: 16 bytes, first 0x67
offset 0: 16 bytes, first 0x07
offset 1048560: 16 bytes, first 0x17
offset 349520: 16 bytes, first 0xb7
```

The same program works unchanged with `/vsisubfile/1000_4000,/data/a.bin`,
`hf://datasets/owner/name/file` or, with `AWS_NO_SIGN_REQUEST=YES` in the environment,
a public `s3://bucket/key`.

## 8. Blocking fetch and per-batch options

- `karu_client_fetch(client, requests, count)` requires a buffer in every request
  (`karu_client_fetch: request 0 has no destination buffer`), submits, drains and frees
  the batch. It returns `KARU_OK` or the status of the first failed completion, in
  completion order, with its message. After a failure every destination is
  unspecified, including those of reads that succeeded.
- `karu_client_submit_with` and `karu_client_fetch_with` take planner overrides for one
  batch. Concurrency, retries and timeouts are not per batch.

```c
karu_submit_options options = KARU_SUBMIT_OPTIONS_INIT; // struct_size plus KARU_INHERIT
options.coalesce_gap = 0;                               // no merging in this batch
options.coalesce_limit = 8u << 20;                      // or cap merged spans at 8 MiB
karu_status status = karu_client_fetch_with(client, requests, count, &options);
```

| Field | Meaning | Rule |
| --- | --- | --- |
| `struct_size` | `sizeof(karu_submit_options)` of the header you compiled | must cover `coalesce_gap`; later fields beyond it inherit |
| `coalesce_gap` | largest gap merged, in bytes | `0` disables merging |
| `coalesce_limit` | largest merged span | at least 1 |
| `coalesce_parts` | most requests in one transfer | at least 1 |
| `coalesce_amplification` | most transferred bytes per requested byte | at least 1 |

`KARU_INHERIT` (`UINT64_MAX`) keeps the client value. Violations fail with
`KARU_ERR_INVALID`, for example `karu_client_submit_with: coalesce_limit must be at least 1`.

## 9. Credential callbacks

```c
typedef karu_status (*karu_credentials_provider)(void* user_data, karu_credentials_kind kind,
                                                 const char* canonical_path,
                                                 karu_credentials* out);
```

| Kind | Fields Karu uses |
| --- | --- |
| `KARU_CREDENTIALS_AWS` | `access_key_id`, `secret_access_key`, optional `session_token` |
| `KARU_CREDENTIALS_SOURCE` | the same fields, in a separate cache namespace |
| `KARU_CREDENTIALS_GCS` | `bearer_token`, or `access_key_id` plus `secret_access_key` as HMAC keys |
| `KARU_CREDENTIALS_AZURE` | `sas_token`, `bearer_token`, or `account_name` plus base64 `secret_access_key`; empty fields fall back to the options |

- The callback replaces native discovery for its kind. An explicit
  `*_NO_SIGN_REQUEST=YES` still wins and skips it.
- For submitted reads it runs on Karu's credential workers, possibly concurrently for
  different paths. During `karu_client_size` it runs synchronously on the calling thread.
  It and `user_data` must therefore be thread safe. It must not call into the same client,
  and it must bound its own I/O because Karu cannot interrupt it.
- Karu copies every string before the callback returns. `release(user_data)` runs once
  nothing holds the callback any more: no builder, client or live batch.
- Return `KARU_OK`, or a failure: negative statuses pass through, anything else becomes
  `KARU_ERR_CREDENTIALS` (`<path>: custom credential provider returned <status>`).
- `cache_prefix` (a canonical VSI or URI prefix containing the requested path) enables
  reuse under that prefix. Without it the callback runs for every transfer and size
  probe that needs credentials; simultaneous lookups for the same path share one call.
  `expires_at` is Unix seconds, or 0 for no expiry. Expired values are rejected. Refresh
  rules are in `credentials.md`.

Example, run against `tests/http_server.py` (argv[1] is the loopback port):

```c
#include <karu/karu.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

typedef struct {
    atomic_int calls;
} provider_state;

static karu_status aws_provider(void* user_data, karu_credentials_kind kind,
                                const char* canonical_path, karu_credentials* out) {
    provider_state* state = user_data;
    atomic_fetch_add(&state->calls, 1);
    (void)canonical_path; // for example "/vsis3/bucket/object"
    if (kind != KARU_CREDENTIALS_AWS)
        return KARU_ERR_CREDENTIALS;
    out->access_key_id = "example-key-id"; // copied by Karu
    out->secret_access_key = "example-secret";
    out->session_token = NULL;
    out->cache_prefix = "/vsis3/bucket/"; // valid for every object in the bucket
    out->expires_at = (int64_t)time(NULL) + 3600;
    return KARU_OK;
}

enum { COUNT = 8, LENGTH = 32 };

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    char endpoint[64];
    snprintf(endpoint, sizeof endpoint, "http://127.0.0.1:%s", argv[1]);

    provider_state state = {0};
    karu_config* config = NULL;
    karu_client* client = NULL;
    karu_locator* object = NULL;
    unsigned char buffers[COUNT][LENGTH];
    karu_req requests[COUNT];
    int failed = 1;

    if (karu_config_create_empty(&config) != KARU_OK ||
        karu_config_set_option(config, "AWS_S3_ENDPOINT", endpoint) != KARU_OK ||
        karu_config_set_option(config, "AWS_VIRTUAL_HOSTING", "NO") != KARU_OK ||
        karu_config_set_credentials_provider(config, KARU_CREDENTIALS_AWS, aws_provider, &state,
                                             NULL) != KARU_OK ||
        karu_client_create(config, &client) != KARU_OK ||
        karu_resolve("s3://bucket/object", &object) != KARU_OK) {
        fprintf(stderr, "setup: %s\n", karu_last_error());
        goto cleanup;
    }

    for (size_t index = 0; index < COUNT; ++index)
        requests[index] = (karu_req){object, index * 256, LENGTH, buffers[index], NULL, NULL};
    karu_submit_options options = KARU_SUBMIT_OPTIONS_INIT;
    options.coalesce_gap = 0; // eight separate transfers for this batch only
    const karu_status fetched = karu_client_fetch_with(client, requests, COUNT, &options);
    if (fetched != KARU_OK) {
        fprintf(stderr, "fetch: %s: %s\n", karu_status_string(fetched), karu_last_error());
        goto cleanup;
    }
    printf("%d reads, provider called %d time(s), first byte 0x%02x\n", COUNT,
           atomic_load(&state.calls), buffers[1][0]);
    failed = 0;

cleanup:
    karu_locator_free(object);
    karu_client_free(client);
    karu_config_free(config);
    return failed;
}
```

```text
$ python3 tests/http_server.py ./provider
8 reads, provider called 1 time(s), first byte 0x07
```

## 10. Threads and fork

- One engine per client: one I/O thread driving libcurl multi, 4 credential workers and
  4 file workers. They start on first use and stop when the client and all its batches
  are freed.
- `karu_client_size` runs the entire size probe on the calling thread, including native
  discovery, a custom credential callback and any `credential_process`. Submitted cloud
  reads use credential workers instead. There is no background refresh: credentials are
  renewed lazily by the first request that needs them.
- `karu_client_submit`, `karu_client_fetch` and `karu_client_size` may be called from
  many threads on one client.
- `fork()`: a client used in a child notices the new process id, abandons the inherited
  engine without destroying it, and builds a fresh one. This makes clients safe to
  create before worker processes are forked (PyTorch `DataLoader` with `num_workers > 0`).
  Batches do not cross a fork: drain and free them in the process that submitted them,
  and never call `karu_batch_next` or `karu_batch_free` on an inherited batch.

## 11. Writing bindings

- Cache one client per configuration and share it. `karu_client_matches_config` tells a
  binding whether an existing client can serve a newly built configuration.
- Release the host language lock (the Python GIL, for example) around
  `karu_batch_next` with a nonzero timeout, `karu_client_fetch`, `karu_client_size` and
  `karu_batch_free`; all of them can block.
- Pass memory the host owns (NumPy arrays, R raw vectors, Julia arrays) as `buffer` and
  keep it pinned until `karu_batch_free` returns. Alternatively pass `NULL` and adopt
  `done.buffer` with a finalizer that calls `karu_free`.
- Callbacks run on Karu threads: reacquire the host lock inside the callback, never
  re-enter the client, and time out host-side I/O.
- Map statuses to exceptions with the table in section 3, reading `karu_last_error()`
  on the same thread immediately after the failing call.
- Initialize `karu_submit_options` with `KARU_SUBMIT_OPTIONS_INIT` so `struct_size`
  matches the header the binding was compiled with.
