# Karu repository guidance

Karu is a C++23 library that reads exact byte ranges from local files, HTTP, S3, GCS,
Azure, Hugging Face and Source Cooperative through libcurl. Its stable boundary is the C
ABI in `include/karu/karu.h`; `include/karu/karu.hpp` is a header-only C++ facade over it.

## Working agreements

- Keep Karu format free and read only. It resolves addresses, reports sizes and reads
  byte ranges. Format parsing, writes, listings, archives and object caches belong to the
  layers above it (rumi, cozip, GDAL).
- Keep the hot path batch oriented. Do not add per-file or per-range blocking work, and
  keep credential discovery, callbacks and helper processes off the I/O thread.
- The C header is the binding surface for Python, R and Julia. C++ types and exceptions
  stop at `src/karu.cpp`. Changing `karu_req`, `karu_done` or `karu_credentials` breaks
  the ABI; `karu_submit_options` grows through `struct_size`.
- Build requests late and per attempt. Locators never hold endpoints or secrets, signed
  requests never follow redirects, user headers never cross origins, and messages redact
  URLs.
- Treat `build/` and `build-*/` as build artifacts. `docs/` is hand-written HTML, and
  `CONFIGURATION.md` and `docs/configuration.html` describe the same options.
- A new or changed option goes into `src/config_options.hpp`, `CONFIGURATION.md`,
  `docs/configuration.html` and `CHANGELOG.md` together.
- Tests stay hermetic: use `karu_config_create_empty()` or `ConfigBuilder(false)` and
  temporary trees, and never read real credentials.

## Validation

- `make test` builds with warnings as errors and runs the component, C API, loopback HTTP
  and credential suites (the last two need Python 3). If CMake cannot find OpenSSL on
  macOS, add `CMAKE_FLAGS=-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)`.
- Run `make test-asan` and `make test-tsan` for changes to the engine, batches,
  transport, the credential cache or process handling, and `make package-test` when
  headers or CMake packaging change.
- Run the storage emulators (`tests/emulators/up.sh`, then
  `ctest --test-dir build -R karu_emulators`) for signing and protocol changes.
- Run `make format` before committing C and C++ changes.

Detailed usage, path grammar, configuration, credential, read-path, repository and
debugging procedures live in the repository's `karu` skill. Load the relevant reference
from that skill instead of expanding this always-on file with task-specific instructions.
