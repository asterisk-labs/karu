# Storage emulators

`tests/credential_server.py` answers whatever Karu asks it, because we wrote it.
It can prove that Karu parses a response, never that Karu produces a request a
real service would accept.

These three emulators are independent implementations of the protocols. They
verify our signatures and compose their own responses, so a signing or range
mistake is a rejected request instead of a passing test.

| Backend | Emulator | Needs |
|---|---|---|
| S3, Source | [MinIO](https://min.io) | a container runtime |
| Azure Blob | [Azurite](https://github.com/Azure/Azurite) | Node only |
| GCS | [fake-gcs-server](https://github.com/fsouza/fake-gcs-server) | a container runtime |

## Running them

```sh
tests/emulators/up.sh          # start and seed all three
ctest --test-dir build -R karu_emulators
tests/emulators/down.sh
```

`up.sh azure` starts only Azurite, which is useful on a machine with no
container runtime. Anything that does not start is left down, and
`karu_emulators` skips the backends it cannot reach. When none of them answers
the test reports ctest's skip status, so `make test` is unaffected.

CI sets `KARU_TEST_REQUIRE_ALL_EMULATORS=1`, which turns a missing backend into
a failure during both startup and the test itself. Local runs remain
best-effort.

The emulator versions are pinned in `up.sh`. This keeps registry migrations and
protocol-routing changes in a new upstream `latest` image from silently changing
the CI environment.

## Endpoints

Override any of these if the defaults collide with something you already run:

| Variable | Default |
|---|---|
| `KARU_TEST_S3_ENDPOINT` | `127.0.0.1:9000` |
| `KARU_TEST_AZURE_ENDPOINT` | `http://127.0.0.1:10000/devstoreaccount1` |
| `KARU_TEST_GCS_ENDPOINT` | `http://127.0.0.1:4443` |
| `KARU_TEST_S3_PORT` | `9000` |
| `KARU_TEST_AZURE_PORT` | `10000` |
| `KARU_TEST_GCS_PORT` | `4443` |
| `KARU_TEST_S3_KEY_ID` | `karuemulator` |
| `KARU_TEST_S3_SECRET` | `karuemulator-secret` |
| `KARU_EMULATOR_STATE` | `$TMPDIR/karu-emulators` |

The Azure account is Azurite's documented development account, and the MinIO
credentials are fixed in `up.sh`. None of them is a secret.

## Seeding

Every backend serves the same 64 KiB object at `karu/fixture.bin`, produced by
`fixture.py` and rebuilt byte for byte inside `tests/test_emulators.cpp`.

Each one is seeded the way that involves the least code of ours:

- **S3** through MinIO's own `mc` client, so no signing of ours stands between
  the fixture and the server.
- **GCS** by mounting a directory, because fake-gcs-server serves whatever it
  finds under `/data` with one directory per bucket.
- **Azure** by `seed_azure.py`, which signs from the Shared Key specification.
  There is no client to borrow, so this is a second implementation that Azurite
  has to accept before any test runs.
