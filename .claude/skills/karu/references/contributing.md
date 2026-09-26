# Working on the karu repository

For changes inside `github.com/asterisk-labs/karu`. Sources: `CMakeLists.txt`,
`tests/CMakeLists.txt`, `Makefile`, `.github/workflows/`, `tests/emulators/README.md`,
`.clang-format`, `CHANGELOG.md`, the test suites.

## Contents

1. Repository map
2. Build and test commands
3. Test suites
4. Loopback fixtures and storage emulators
5. Adding an option
6. Adding or changing a backend
7. Design rules
8. Conventions
9. CI, releases and security

## 1. Repository map

```text
include/karu/karu.h            C ABI: the stable boundary and the surface for bindings
include/karu/karu.hpp          header-only C++23 facade over the C ABI
src/karu.cpp                   C entry points, argument checks, exceptions to statuses
src/uri.{hpp,cpp}              URI and VSI grammar, windows, canonical identity (no I/O)
src/locator.hpp                karu_locator wraps a Resolved
src/config.{hpp,cpp}           ConfigBuilder, ConfigSnapshot, precedence, validation
src/config_options.hpp         every accepted option name; order = environment alias precedence
src/client.hpp                 karu_client: snapshot plus lazily created Engine, fork detection
src/request_builder.{hpp,cpp}  signed or anonymous decision; per-attempt request materialization
src/credential_cache.{hpp,cpp} scoped credential cache, single flight, refresh, invalidation
src/error.{hpp,cpp}            thread-local 512-byte message buffer
src/platform.{hpp,cpp}         process id, timegm, UTF-8 paths, positional file reads
src/process.{hpp,cpp}          credential_process runner (posix_spawn, CreateProcess plus job object)
src/text.hpp                   concat, redact_url
src/runtime/planner.*          validation, grouping, coalescing into Transfer and Part
src/runtime/engine.*           event loops, file and credential workers, retries, cancellation, size
src/runtime/transport.*        curl easy setup, header and body callbacks, scatter, size probe
src/runtime/http_response.*    status mapping, transient classification, Retry-After, backoff
src/runtime/batch.*            per-batch completion queue
src/runtime/{transfer,deadline,curl_types}.hpp   transfer state, monotonic deadline, curl RAII
src/backends/contract.hpp      CloudProvider contract; registry.cpp maps Backend to provider
src/backends/{s3,gcs,azure,source}.cpp            request preparation and signing
src/backends/{s3,gcs,azure,source}_credentials.*, aws_profile.*   native discovery chains
src/backends/{http,hugging_face}.cpp              unsigned backends
src/backends/{credentials,url,crypto,s3_signing}.*   shared helpers
tests/                         suites and fixtures (section 3)
benchmarks/coalescing.cpp      planner benchmark, CSV output
cmake/karuConfig.cmake.in      installed package configuration
docs/                          hand-written static site: index, how-karu-reads, configuration
CONFIGURATION.md               option reference, installed with the package
VERSION, CHANGELOG.md          single version source; Keep a Changelog history
SECURITY.md                    private vulnerability reporting
```

## 2. Build and test commands

Requirements: CMake 3.21, a C++23 compiler (CI: GCC on Ubuntu 24.04, Clang 19, AppleClang
on macOS 15, MSVC 2022), libcurl 7.83.0 or newer, OpenSSL 3, threads. Python 3 is needed
for the loopback HTTP and credential suites; without it CMake skips registering them.

```sh
make test                                   # configure build/ with -Werror, build, ctest
make test CMAKE_FLAGS=-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)   # if macOS cannot find OpenSSL
ctest --test-dir build -R karu_http --output-on-failure              # one suite
```

| Target | What it does |
| --- | --- |
| `make` / `make build` | configure `BUILD` (default `build`) with tests and `compile_commands.json`, then build |
| `make test` | build and run every registered ctest |
| `make debug`, `make release` | `test` in `build-debug` or `build-release` |
| `make install` | install into `PREFIX` (default `build/stage`) |
| `make package-test` | install, then build and test `tests/package` against the installed package |
| `make test-asan` | Debug build with `KARU_SANITIZE=address,undefined` in `build-asan` |
| `make test-tsan` | Debug build with `KARU_SANITIZE=thread` in `build-tsan` |
| `make coverage` | GCC `--coverage` build in `build-coverage`, `gcovr` reports for `src/` |
| `make benchmark` | build and run `karu_coalescing_benchmark` in `build-benchmarks` |
| `make check` | `test`, `package-test`, `test-asan`, `test-tsan` |
| `make format`, `make format-check` | clang-format on changed or untracked C and C++ files only |
| `make clean` | remove the Makefile's build directories |

Variables: `BUILD`, `BUILD_TYPE` (Release), `JOBS`, `WERROR` (ON), `PREFIX`, `CMAKE_FLAGS`.
CMake options: `KARU_BUILD_TESTS`, `KARU_BUILD_BENCHMARKS`, `KARU_WERROR`,
`KARU_COVERAGE` (GCC only), `KARU_SANITIZE` (ignored with MSVC). Tests and benchmarks are
off by default for consumers.

## 3. Test suites

| ctest name | Binary and driver | Guards |
| --- | --- | --- |
| `karu` | `karu_tests` (`test_main.cpp` plus `test_uri_config`, `test_s3`, `test_gcs`, `test_azure`, `test_hugging_face`, `test_source`, `test_planner_property`, `test_status_and_paths`, `test_engine`) | URI grammar and windows, option precedence, exact SigV4, GOOG1 and Shared Key requests, managed headers, callback cache, local engine, batch lifetimes and cancellation under threads, planner properties, status mapping, the C++ facade |
| `karu_c_api` | `karu_c_api_test` from `test_c_api.c` | the header compiles as C11 and the ABI works from C |
| `karu_http` | `tests/http_server.py` runs `karu_http_test` with its port | real HTTP: ranges, chunked bodies, retries and `Retry-After`, redirects and header leaks, S3 region and credential corrections, preconditions, ignored ranges, truncation, size probes, connection reuse, cancellation |
| `karu_credentials` | `tests/credential_server.py` runs `karu_credentials_test` | AWS, GCS, Azure and Source discovery chains, profiles, `credential_process`, STS, OAuth, metadata services, transport options |
| `karu_emulators` | `karu_emulator_test` | reads from MinIO, Azurite and fake-gcs-server; exits 77 (ctest skip) when none answer |
| `karu_package_consumer`, `karu_c_package_consumer` | `tests/package`, run by `make package-test` | `find_package(karu)` from C++ and C |

- There is no external test framework. `tests/test_support.hpp` defines `OK`, `EQ`,
  `EQS` and `SECTION`, and declares every suite function; `test_main.cpp` (or
  `test_credentials_main.cpp`) calls them. A new suite function needs both the
  declaration and the call. Binaries print `N checks, M failures` and exit nonzero on
  failure; `test_http.cpp` and `test_emulators.cpp` have their own small `check`.
- Tests are hermetic: `karu_config_create_empty()`, `ConfigBuilder(false)`, temporary
  trees (`TempTree`) and `ScopedEnvironment` for tests about the environment layer. Never
  read the developer's real `~/.aws`, gcloud or Hugging Face files.
- The credential binary is also its own `credential_process` helper
  (`credential_process_command("ok"|"incomplete"|"fail"|"flood"|"hang"|"no-key")`), the
  only command spelling that behaves the same under `sh` and `cmd.exe`.
- Portable expectations: Schannel on Windows answers `CURLE_NOT_BUILT_IN` for
  `CURLOPT_CAPATH`, and a libcurl without HTTP/2 rejects the HTTP/2 versions at setopt.
  Tests for those options accept either outcome but check the option was wired.
- Under UBSan, a value outside an unscoped enum's range is undefined behaviour; keep
  out-of-range status tests inside the enum's implied range.

Running binaries directly:

```sh
build/tests/karu_tests
python3 tests/http_server.py build/tests/karu_http_test
python3 tests/credential_server.py build/tests/karu_credentials_test
```

## 4. Loopback fixtures and storage emulators

- `tests/http_server.py` starts two threaded HTTP/1.1 servers (the second is a
  cross-origin redirect target) and runs the test binary with the port. Routes are named
  after scenarios (`/missing`, `/retry`, `/ignore-range`, `/redirect-cross-origin`,
  `/bucket/region`, `/bucket/credential-refresh`, ...). Object byte `i` is
  `(i * 31 + 7) & 0xFF`. To cover a new HTTP behaviour, add a route there and a check in
  `test_http.cpp`.
- `tests/credential_server.py` answers STS, container, IMDS, OAuth, metadata and App
  Service requests deterministically and records request bodies; timestamps are computed
  per request so "valid" is always in the future.
- These fixtures accept whatever Karu sends. Only the emulators check that real services
  would accept the signatures:

```sh
tests/emulators/up.sh          # all; or: up.sh s3 | up.sh azure | up.sh gcs
ctest --test-dir build -R karu_emulators --output-on-failure
tests/emulators/down.sh
```

- S3 and Source use MinIO: run `tests/emulators/build_minio.sh` once with Go 1.24+.
  GCS uses fake-gcs-server (Docker); Azure uses Azurite (Node).
  Versions are pinned in the scripts. CI caches the MinIO binaries.
- Every emulator serves the same 64 KiB `karu/fixture.bin`. MinIO is seeded with its own
  `mc`, GCS by mounting a directory, and Azure by `seed_azure.py`, an independent Shared
  Key signer.
- `KARU_TEST_REQUIRE_ALL_EMULATORS=1` (set in CI) turns a missing backend into a failure.
  Ports, endpoints and keys can be overridden with `KARU_TEST_*` variables listed in
  `tests/emulators/README.md`.
- `azurite-blob` is a binary inside the `azurite` package:
  `npx --yes --package=azurite@3.37.0 azurite-blob`. On a cold npm cache the download
  outlasts the startup wait, which is why `up.sh` fetches it first.

## 5. Adding an option

1. Add the name to its group in `src/config_options.hpp`. The order inside a group is the
   environment alias precedence.
2. For an alias, fold it into the canonical name in `canonical_option_name`
   (`src/config.cpp`).
3. Validate it: booleans and enumerations in `validate_values`; numeric client options in
   `ConfigBuilder::freeze` with a field in `ClientOptions`. Client-wide options start
   with `KARU_`, which also forbids them in path rules.
4. Read it where it is used with `config.option(path, "NAME", default)`.
5. If it selects or supplies credentials, add it to the provider's `CREDENTIAL_OPTIONS`
   in `src/backends/{s3,gcs,azure,source}.cpp`. That list defines the credential cache
   scope and, for Source, switches signing on.
6. Document it in `CONFIGURATION.md` and `docs/configuration.html` (kept in sync by hand)
   and add a `CHANGELOG.md` entry.
7. Test precedence in `test_uri_config.cpp` and behaviour in the provider or HTTP suite.

## 6. Adding or changing a backend

- Cloud providers implement `CloudProvider` in `src/backends/contract.hpp`: backend,
  `karu_credentials_kind`, no-sign option, whether anonymous is the default, credential
  options, a native loader and `prepare_request`. Register it in `registry.cpp`.
- A new backend also needs: a `Backend` value and grammar in `src/uri.cpp` (URI scheme,
  VSI alias table and the unsupported-handler message), the scheme mapping in
  `canonical_vsi_path`, an option group, sources in `CMakeLists.txt`, the credential
  rejection rule in `http_response.cpp` (`credentials_expired`), a new
  `karu_credentials_kind` in `karu.h` (an ABI addition to record in the changelog), a
  request test with fixed signature vectors, loopback routes, and an emulator when one
  exists.
- A prepared request must set `http.follow_redirects = false` whenever credentials or a
  signature are bound to it.
- Only stores that serve byte ranges belong here. Listing, uploads, archives and
  in-memory filesystems are out of scope.

## 7. Design rules

- **Format free and read only.** Karu never parses TIFF, Zarr, rumi or archives and never
  lists or writes. Planners live above it (rumi `plan_ranges`, container manifests that
  emit `/vsisubfile/` paths). GeoTIFF support means addressing only: Karu fetches the
  bytes and GDAL decodes them from memory.
- **Batches first.** The workload is on the order of a million ranges for a data loader,
  and concurrency is the lever. Nothing on the hot path may block per file or per range;
  the facade's `read` is a convenience over `fetch`.
- **Stateless reads.** No object byte or size cache. Connections, TLS sessions and
  credentials are reused. libcurl's DNS cache is disabled for address shuffling.
- **Blocking work off the event loops.** Submitted-read discovery, callbacks and helper
  processes run on credential workers; files run on file workers. Synchronous size probes
  do their credential work on the calling thread.
- **Late, per-attempt requests.** Locators hold no endpoints or secrets; requests are
  rebuilt and signed for each attempt.
- **Safe transport.** libcurl is mandatory (7.83.0 or newer stops forwarding
  `Authorization` on cross-origin redirects, CVE-2022-27776). Signed requests never follow
  redirects, user headers never cross origins, protocols are HTTP and HTTPS only, and
  messages redact URLs.
- **A C ABI for bindings.** Python, R and Julia bindings should use `karu.h`; C++ types
  and exceptions stop at `src/karu.cpp`.
- **Fork safety.** `karu_client` compares the process id and abandons, never destroys,
  inherited engines and mutexes, because `DataLoader` workers fork.

## 8. Conventions

- C++23. `.clang-format`: LLVM base, 4-space indent, 100 columns, left pointer alignment;
  run `make format` before committing.
- Warnings: `-Wall -Wextra -Wpedantic -Wshadow` (MSVC `/W4 /permissive- /utf-8`), errors in
  CI through `KARU_WERROR=ON`.
- RAII everywhere: curl handles in `unique_ptr` with deleters (`curl_types.hpp`),
  transfers owned by `unique_ptr` in every queue. Fallible internals return
  `std::expected`; messages are built with `concat`.
- POSIX and Windows differences live in `platform.cpp` and `process.cpp`. Windows builds
  define `NOMINMAX` and `WIN32_LEAN_AND_MEAN`.
- Comments explain why, in full sentences.
- `CHANGELOG.md` follows Keep a Changelog: an `Unreleased` section with `Added`,
  `Changed` and `Fixed`, written for users.
- Commit subjects are short and imperative (`make empty redirect test backend-neutral`,
  `Use posix_spawn for POSIX credential helpers`).

## 9. CI, releases and security

`ci.yml`, on pushes to `main` and pull requests:

- Build matrix: Linux GCC, Linux Clang 19, macOS AppleClang (Homebrew `openssl@3`),
  Windows MSVC (vcpkg `curl` and `openssl`). Release, tests and benchmarks built,
  `KARU_WERROR=ON`, ctest, install, then the package consumer test.
- The Linux GCC job also starts the emulators with `KARU_TEST_REQUIRE_ALL_EMULATORS=1`
  and runs `karu_emulators`, printing emulator logs on failure.
- Sanitizers: Debug builds with ASan plus UBSan and with TSan on Linux.
- Coverage: `make coverage`, uploaded to Codecov from `main`.

`release.yml`, on `v*` tags:

- The tag must equal `v` plus `VERSION`.
- Linux x86_64, macOS arm64 and Windows x86_64 are built, tested and installed into
  archives (`karu-<version>-<platform>.tar.gz` or `.zip`), published with `SHA256SUMS`
  as a GitHub release with generated notes.
- Before tagging: bump `VERSION`, move `Unreleased` into a dated section with a compare
  link, update the version label in the `docs/*.html` headers, and update the version line
  in this skill's `SKILL.md` (the `karu_agent_skill` test fails until it matches).

Security: report privately through GitHub security advisories
(`https://github.com/asterisk-labs/karu/security/advisories/new`). Replace credentials,
tokens, signed URLs, account identifiers and private bucket names with synthetic values
before sharing logs.
