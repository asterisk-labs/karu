# Changelog

Notable user-visible changes are recorded here.

## [Unreleased]

## [0.2.0] - 2026-09-10

### Added

- An optional planner benchmark records transfer counts, byte amplification and
  planning cost for contiguous and sparse range batches.
- C and C++ callers can override coalescing for one submitted batch without
  rebuilding the client.
- Karu sends `karu/<version>` as its default User-Agent. Set
  `KARU_HTTP_USER_AGENT` to replace it or to an empty value to omit it.
- CI measures line coverage of the C++ core and checks exact AWS SigV4, GCS
  GOOG1 and Azure SharedKey signatures against fixed vectors.
- The loopback integration suite covers range reads, size probes,
  preconditions, error mapping and credential refresh for every backend.

### Changed

- The C and C++ APIs now define which bytes may be inspected after a failed or
  short read.
- Merged HTTP ranges are scattered directly into their destination buffers;
  gaps no longer allocate staging memory or add a second copy.
- Local batches only merge ranges that overlap or touch.
- HTTP and GCS configuration use Karu-owned names. `KARU_HTTP_*` replaces the
  old transport namespace, while GCS options consistently begin with `GCS_`.
- URI paths, chaining and the C API remain compatible with 0.1.
- Credential discovery and refresh run outside the network scheduler.
  Independent paths may resolve concurrently; simultaneous requests for one
  scope share a single refresh.
- Temporary credentials refresh during the last ten percent of their lifetime,
  within a 1–60 second window. An authentication rejection invalidates the
  cached value and retries once with fresh credentials.
- Range reads and size probes now share response-state reset, `Retry-After`
  parsing and backoff behavior.

### Fixed

- Path-specific options and credential scopes stop at path boundaries, so a
  rule for `s3://data` cannot apply to `s3://data-private`.
- AWS and Source `credential_process` helpers are terminated at
  `KARU_REQUEST_TIMEOUT` and cannot return unbounded output.
- Concurrent credential lookups keep their shared refresh state alive until
  every waiting caller has resumed.
- Expired credentials are rejected instead of entering the cache.
- HTTP-date values in `Retry-After` are honored by size probes.

## [0.1.1] - 2026-09-09

### Fixed

- Linux discovers the host system CA bundle at runtime instead of retaining a
  certificate path from the build environment.

## [0.1.0] - 2026-09-09

First public release.

[Unreleased]: https://github.com/asterisk-labs/karu/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/asterisk-labs/karu/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/asterisk-labs/karu/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/asterisk-labs/karu/releases/tag/v0.1.0
