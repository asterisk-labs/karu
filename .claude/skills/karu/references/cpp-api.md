# C++ facade

`include/karu/karu.hpp` is a header-only C++23 layer over the C ABI. It adds move-only
RAII handles, spans, `std::optional` planner overrides and `std::expected` results. It
does not create another engine: every call reaches the same client, batch and completion
machinery described in `c-api.md`. The example below compiles with
`-Wall -Wextra -Wpedantic -Werror` and ran against karu 0.2.3 on local files, `hf://`
and a public `s3://` bucket.

## Contents

1. Results and errors
2. `Config`
3. `Object`
4. `Client`
5. `Read`, `SubmitOptions`, `Batch` and `BatchEvent`
6. Example
7. Differences from the C API

## 1. Results and errors

```cpp
namespace karu {
struct Error { karu_status status = KARU_OK; std::string message; };
template <typename Value> using Result = std::expected<Value, Error>;
}
```

- Every fallible member returns `Result<T>` and is `[[nodiscard]]`.
- `Error::message` is a copy of `karu_last_error()` taken right after the failing call,
  so it survives later Karu calls.
- `.value()` throws `std::bad_expected_access<karu::Error>`. It suits examples and
  scripts; library code should test the result.
- `karu_status_string(error.status)` gives the short name (`"credentials unavailable"`).

## 2. `Config`

| Member | Notes |
| --- | --- |
| `static Result<Config> from_environment()` | `karu_config_create`: snapshots the environment, enables default discovery |
| `static Result<Config> empty()` | `karu_config_create_empty`: hermetic |
| `Result<void> set(name, value)` / `unset(name)` | unknown names fail at once; values are checked by `Client::create` |
| `Result<void> set_path(prefix, name, value)` / `unset_path(prefix, name)` | segment-aware prefix rule; `KARU_*` names are rejected |
| `Result<void> set_credentials_provider(kind, provider, user_data, release = nullptr)` | C callback; see `credentials.md` |
| `Result<void> clear_credentials_provider(kind)` | removes the callback for `kind` |
| `karu_config* native_handle() const` | for C calls; ownership stays with the object |

Move only. A `Config` is a mutable builder: do not share one between threads without a
lock. Clients copy it, so it may be destroyed after `Client::create`.

## 3. `Object`

| Member | Notes |
| --- | --- |
| `static Result<Object> parse(std::string_view uri)` | `karu_resolve`: no I/O; `KARU_ERR_URI` or `KARU_ERR_UNSUPPORTED` |
| `std::string_view uri() const` | canonical identity; valid while the object lives |
| `bool is_remote() const` | false only for local files |
| `std::uint64_t window_offset() const`, `window_length() const` | window; `KARU_TO_END` when unbounded |
| `const karu_locator* native_handle() const` | for C calls |

Move only and immutable. A `Read` points at an `Object`, but submission copies what it
needs, so the object may be destroyed after `submit` or `fetch` returns.

## 4. `Client`

| Member | Notes |
| --- | --- |
| `static Result<Client> create(const Config&)` | validates and freezes; invalid values give `KARU_ERR_CONFIG` |
| `Result<bool> matches(const Config&) const` | `karu_client_matches_config` |
| `Result<std::uint64_t> size(const Object&) const` | visible size; one `GET` of byte 0 for remote objects, never cached |
| `Result<void> read_into(object, offset, std::span<std::byte>, if_match = {}) const` | one blocking read into caller memory; an empty span succeeds without I/O |
| `Result<std::vector<std::byte>> read(object, offset, length, if_match = {}) const` | allocates `length` zeroed bytes, then `read_into`; length 0 gives an empty vector |
| `Result<void> fetch(std::span<const Read>, const SubmitOptions& = {}) const` | blocking batch; fails with the first failed read; destinations unspecified on failure |
| `Result<Batch> submit(std::span<const Read>, const SubmitOptions& = {}) const` | returns at once; drain with `Batch::next` |
| `int concurrency() const`, `std::uint64_t coalesce_gap() const`, `int max_attempts() const` | frozen values |
| `karu_client* native_handle() const` | for C calls |

Move only; all members are `const` and safe to call from many threads at once. Keep one
client for the life of the process or of the configuration.

## 5. `Read`, `SubmitOptions`, `Batch` and `BatchEvent`

```cpp
struct Read {
    const Object* object = nullptr;      // must not be null
    std::uint64_t offset = 0;            // relative to the object's window
    std::span<std::byte> destination;    // must not be empty; alive until the Batch dies
    void* tag = nullptr;                 // returned in BatchEvent::tag
    std::string_view if_match;           // optional ETag, copied at submit
};

struct SubmitOptions {                   // unset fields inherit the client settings
    std::optional<std::uint64_t> coalesce_gap;           // 0 disables merging
    std::optional<std::uint64_t> coalesce_limit;
    std::optional<std::uint64_t> coalesce_parts;
    std::optional<std::uint64_t> coalesce_amplification;
};

enum class BatchState { ready, end, timeout };

struct BatchEvent {
    BatchState state = BatchState::end;
    void* tag = nullptr;
    karu_status status = KARU_OK;        // result of this one read
    std::uint64_t bytes_read = 0;        // defined prefix of the destination
    std::string message;                 // filled only when status != KARU_OK
};
```

- Designated initializers must follow member order: `object`, `offset`, `destination`,
  `tag`, `if_match`.
- The request length is `destination.size()`.
- `Batch::next(int timeout_ms = -1)` returns `Result<BatchEvent>`. The `Result` fails only
  when the call itself fails. A read that failed is a `ready` event with
  `status != KARU_OK` and a `message`. `timeout` means nothing was ready; `end` means
  every read has been reported.
- `bytes_read` equals the destination size on success; `KARU_ERR_RANGE` can report a
  shorter valid prefix at the end of an object.
- Several threads may call `next` on one `Batch`. Destroying the `Batch` cancels pending
  work and blocks until workers release the destinations; do not destroy it while
  another thread is inside `next`.

## 6. Example

```cpp
// Reads the first and last 64 bytes of an object, once blocking and once as a batch.
#include <karu/karu.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

int fail(const char* step, const karu::Error& error) {
    std::cerr << step << ": " << karu_status_string(error.status) << ": " << error.message << '\n';
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " PATH_OR_URI\n";
        return 2;
    }

    auto config = karu::Config::from_environment();
    if (!config)
        return fail("config", config.error());
    if (auto set = config->set("KARU_CONCURRENCY", "128"); !set)
        return fail("option", set.error());

    auto client = karu::Client::create(*config); // copies the configuration
    if (!client)
        return fail("client", client.error());
    auto object = karu::Object::parse(argv[1]); // no I/O
    if (!object)
        return fail("parse", object.error());

    auto size = client->size(*object); // one request for remote objects, never cached
    if (!size)
        return fail("size", size.error());
    if (*size < 64) {
        std::cerr << "object has only " << *size << " bytes\n";
        return 1;
    }

    // Blocking read into a new vector.
    auto head = client->read(*object, 0, 64);
    if (!head)
        return fail("read", head.error());

    // Batch read: completions arrive in any order and carry the caller's tag.
    std::array<std::array<std::byte, 64>, 2> buffers{};
    const std::array<karu::Read, 2> reads{{
        {.object = &*object, .offset = 0, .destination = buffers[0], .tag = &buffers[0]},
        {.object = &*object, .offset = *size - 64, .destination = buffers[1], .tag = &buffers[1]},
    }};
    auto batch = client->submit(reads);
    if (!batch)
        return fail("submit", batch.error());

    int failed = 0;
    for (;;) {
        auto event = batch->next(); // blocks; pass a timeout in milliseconds to poll
        if (!event)
            return fail("next", event.error());
        if (event->state == karu::BatchState::end)
            break;
        if (event->status != KARU_OK) {
            std::cerr << karu_status_string(event->status) << ": " << event->message << '\n';
            failed = 1;
            continue;
        }
        const bool last = event->tag == &buffers[1];
        std::cout << (last ? "tail" : "head") << ": " << event->bytes_read << " bytes\n";
    }
    std::cout << "blocking read matches batch: " << ((*head)[0] == buffers[0][0] ? "yes" : "no")
              << '\n';
    return failed;
}
```

```text
$ ./example hf://datasets/asterisk-labs/rumi-api-fixtures/data/s2-00-tile.rumi
head: 64 bytes
tail: 64 bytes
blocking read matches batch: yes

$ env -i HOME=/tmp/empty AWS_EC2_METADATA_DISABLED=YES ./example s3://bucket/key
size: credentials unavailable: /vsis3/bucket/key: no AWS credentials; set AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY, install a custom provider, or set AWS_NO_SIGN_REQUEST=YES for a public object
```

Per-batch overrides:

```cpp
const karu::SubmitOptions exact{.coalesce_gap = 0};   // every read becomes its own request
auto fetched = client->fetch(reads, exact);
```

## 7. Differences from the C API

- The facade never lets Karu allocate: an empty `destination` fails with
  `KARU_ERR_INVALID` (`read has an empty destination`), and a null `object` with
  `read has no object`. Use `native_handle()` with `karu_client_submit` and
  `buffer = NULL` if Karu-owned buffers are needed.
- `Client::read` and `read_into` are `fetch` with one request. Their error message is the
  completion message, for example `...: object ended at byte 4096 while reading [4090, +16)`.
- `Batch` has no explicit free; its destructor calls `karu_batch_free`.
- Handles are move only. Moving leaves the source empty; do not use it afterwards.
- The facade adds no locking. `Client` and `Batch::next` are thread safe because the C
  calls are; `Config` is not.
