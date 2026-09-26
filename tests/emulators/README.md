# Storage emulators

These tests check signed requests and range reads against independent storage
emulators. The loopback fixture in `tests/credential_server.py` tests response
handling but does not verify signatures.

| Backend | Emulator | Needs |
|---|---|---|
| S3, Source | [MinIO](https://github.com/minio/minio), built from source | Go 1.24, once |
| Azure Blob | [Azurite](https://github.com/Azure/Azurite) | Node only |
| GCS | [fake-gcs-server](https://github.com/fsouza/fake-gcs-server) | a container runtime |

## Running them

```sh
tests/emulators/build_minio.sh # build MinIO and mc once
tests/emulators/up.sh          # start and seed all three
ctest --test-dir build -R karu_emulators
tests/emulators/down.sh
```

Use `up.sh s3`, `up.sh azure` or `up.sh gcs` to start one backend.
Tests skip unreachable backends. CI sets `KARU_TEST_REQUIRE_ALL_EMULATORS=1`
to fail if any backend is unavailable.

Versions are pinned in `up.sh` and `build_minio.sh`. MinIO and `mc` are built
from pinned source commits and run without Docker. CI caches the binaries.

Both scripts look in `KARU_MINIO_BIN_DIR`, or `${XDG_CACHE_HOME:-$HOME/.cache}/karu-minio`
by default. `build_minio.sh [directory]` can build elsewhere; set
`KARU_MINIO_BIN_DIR` to that directory before running `up.sh`.

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

Fixture setup does not use Karu:

- **S3:** upload with MinIO's `mc` client.
- **GCS:** mount the fixture under `/data/karu/fixture.bin`.
- **Azure:** upload with `seed_azure.py`, an independent Shared Key signer.
