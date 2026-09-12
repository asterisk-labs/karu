#!/bin/sh
# Start the storage emulators the karu_emulators test reads from, and seed each
# one with the same fixture object.
#
# These are independent implementations of the S3, Azure Blob and GCS protocols.
# They verify our signatures and produce their own responses, which is the one
# thing tests/credential_server.py structurally cannot do: that fixture accepts
# whatever Karu sends.
#
# Usage:  tests/emulators/up.sh          start and seed
#         tests/emulators/down.sh        stop and remove
#
# S3 and GCS need a container runtime. Azure does not: Azurite is an npm package
# and runs anywhere Node does, so `up.sh azure` is useful on a laptop with no
# Docker. Anything that fails to start is simply left down, and the test skips
# the backends it cannot reach.
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
STATE="${KARU_EMULATOR_STATE:-${TMPDIR:-/tmp}/karu-emulators}"
WHICH="${1:-all}"

S3_PORT="${KARU_TEST_S3_PORT:-9000}"
AZURE_PORT="${KARU_TEST_AZURE_PORT:-10000}"
GCS_PORT="${KARU_TEST_GCS_PORT:-4443}"
S3_KEY_ID=karuemulator
S3_SECRET=karuemulator-secret

mkdir -p "$STATE"

wait_for() {
    name=$1
    url=$2
    attempt=0
    while [ "$attempt" -lt 60 ]; do
        # Any HTTP answer means the service is listening; the status does not
        # matter, because an unauthenticated probe is expected to be refused.
        if curl -s -o /dev/null --max-time 2 "$url"; then
            echo "  $name listo"
            return 0
        fi
        attempt=$((attempt + 1))
        sleep 1
    done
    echo "  $name NO arrancó" >&2
    return 1
}

have_docker() {
    command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1
}

start_s3() {
    have_docker || { echo "  S3: sin docker, omitido"; return 0; }
    docker rm -f karu-minio >/dev/null 2>&1 || true
    docker run -d --name karu-minio \
        -p "127.0.0.1:$S3_PORT:9000" \
        -e "MINIO_ROOT_USER=$S3_KEY_ID" \
        -e "MINIO_ROOT_PASSWORD=$S3_SECRET" \
        minio/minio:latest server /data >/dev/null
    wait_for "MinIO" "http://127.0.0.1:$S3_PORT/minio/health/live" || return 1
    # Seed through MinIO's own client so no signing code of ours stands between
    # the fixture and the server.
    python3 "$ROOT/fixture.py" > "$STATE/fixture.bin"
    docker run --rm --network host -v "$STATE:/seed" --entrypoint /bin/sh \
        minio/mc:latest -c "
            mc alias set karu http://127.0.0.1:$S3_PORT $S3_KEY_ID $S3_SECRET >/dev/null &&
            mc mb --ignore-existing karu/karu >/dev/null &&
            mc cp /seed/fixture.bin karu/karu/fixture.bin >/dev/null
        " >/dev/null
    echo "  S3 sembrado"
}

start_azure() {
    if ! command -v npx >/dev/null 2>&1; then
        echo "  Azure: sin node, omitido"
        return 0
    fi
    mkdir -p "$STATE/azurite"
    # azurite-blob is a binary inside the "azurite" package, not a package of
    # its own, so npx has to be told which package provides it. Fetch it first,
    # synchronously: on a cold cache the download alone outlasts the wait below,
    # and the service would look like it had failed to start.
    echo "  descargando Azurite si hace falta..."
    if ! npx --yes --package=azurite azurite-blob version >/dev/null 2>&1; then
        echo "  Azure: no se pudo obtener azurite, omitido"
        return 0
    fi
    npx --yes --package=azurite azurite-blob \
        --blobHost 127.0.0.1 --blobPort "$AZURE_PORT" \
        --location "$STATE/azurite" --silent > "$STATE/azurite.log" 2>&1 &
    echo $! > "$STATE/azurite.pid"
    wait_for "Azurite" "http://127.0.0.1:$AZURE_PORT/devstoreaccount1?comp=list" || return 1
    python3 "$ROOT/seed_azure.py"
    echo "  Azure sembrado"
}

start_gcs() {
    have_docker || { echo "  GCS: sin docker, omitido"; return 0; }
    docker rm -f karu-gcs >/dev/null 2>&1 || true
    # fake-gcs-server serves whatever it finds under /data, one directory per
    # bucket, so the fixture needs no upload API and no credentials.
    rm -rf "$STATE/gcs" && mkdir -p "$STATE/gcs/karu"
    python3 "$ROOT/fixture.py" > "$STATE/gcs/karu/fixture.bin"
    docker run -d --name karu-gcs \
        -p "127.0.0.1:$GCS_PORT:4443" \
        -v "$STATE/gcs:/data" \
        fsouza/fake-gcs-server:latest \
        -scheme http -port 4443 -external-url "http://127.0.0.1:$GCS_PORT" >/dev/null
    wait_for "fake-gcs-server" "http://127.0.0.1:$GCS_PORT/storage/v1/b" || return 1
    echo "  GCS sembrado"
}

case "$WHICH" in
    all)   start_s3 || true; start_azure || true; start_gcs || true ;;
    s3)    start_s3 ;;
    azure) start_azure ;;
    gcs)   start_gcs ;;
    *)     echo "uso: $0 [all|s3|azure|gcs]" >&2; exit 2 ;;
esac
