#!/bin/sh
# Start and seed the storage emulators.
# Usage: tests/emulators/up.sh [all|s3|azure|gcs]
# S3 needs build_minio.sh, Azure needs Node, and GCS needs Docker.
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
STATE="${KARU_EMULATOR_STATE:-${TMPDIR:-/tmp}/karu-emulators}"
WHICH="${1:-all}"

S3_PORT="${KARU_TEST_S3_PORT:-9000}"
AZURE_PORT="${KARU_TEST_AZURE_PORT:-10000}"
GCS_PORT="${KARU_TEST_GCS_PORT:-4443}"
S3_KEY_ID="${KARU_TEST_S3_KEY_ID:-karuemulator}"
S3_SECRET="${KARU_TEST_S3_SECRET:-karuemulator-secret}"
# Avoid picking up Midnight Commander's mc from PATH.
MINIO_BIN_DIR="${KARU_MINIO_BIN_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/karu-minio}"
FAKE_GCS_IMAGE="fsouza/fake-gcs-server:1.56.1"
AZURITE_PACKAGE="azurite@3.37.0"

mkdir -p "$STATE"

wait_for() {
    name=$1
    url=$2
    attempt=0
    while [ "$attempt" -lt 60 ]; do
        # Auth errors also confirm the service is listening.
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

unavailable() {
    echo "  $1"
    [ "${KARU_TEST_REQUIRE_ALL_EMULATORS:-0}" != "1" ]
}

start_s3() {
    minio="$MINIO_BIN_DIR/minio"
    mc="$MINIO_BIN_DIR/mc"
    if [ ! -x "$minio" ] || [ ! -x "$mc" ]; then
        unavailable "S3: falta MinIO en $MINIO_BIN_DIR; ejecuta tests/emulators/build_minio.sh" ||
            return 1
        return 0
    fi
    if [ -f "$STATE/minio.pid" ]; then
        kill "$(cat "$STATE/minio.pid")" >/dev/null 2>&1 || true
    fi
    rm -rf "$STATE/minio" "$STATE/mc" && mkdir -p "$STATE/minio"
    MINIO_ROOT_USER="$S3_KEY_ID" MINIO_ROOT_PASSWORD="$S3_SECRET" MINIO_BROWSER=off \
        "$minio" server "$STATE/minio" --address "127.0.0.1:$S3_PORT" \
        > "$STATE/minio.log" 2>&1 &
    echo $! > "$STATE/minio.pid"
    wait_for "MinIO" "http://127.0.0.1:$S3_PORT/minio/health/live" || return 1
    # Seed with mc to keep Karu's signing code out of fixture setup.
    python3 "$ROOT/fixture.py" > "$STATE/fixture.bin"
    export MC_CONFIG_DIR="$STATE/mc"
    "$mc" alias set karu "http://127.0.0.1:$S3_PORT" "$S3_KEY_ID" "$S3_SECRET" >/dev/null &&
        "$mc" mb --ignore-existing karu/karu >/dev/null &&
        "$mc" cp "$STATE/fixture.bin" karu/karu/fixture.bin >/dev/null &&
        "$mc" stat karu/karu/fixture.bin >/dev/null || return 1
    echo "  S3 sembrado"
}

start_azure() {
    if ! command -v npx >/dev/null 2>&1; then
        unavailable "Azure: sin node, omitido" || return 1
        return 0
    fi
    mkdir -p "$STATE/azurite"
    # Fetch before starting the readiness timeout; a cold npm download can exceed it.
    echo "  descargando Azurite si hace falta..."
    if ! npx --yes --package="$AZURITE_PACKAGE" azurite-blob version >/dev/null 2>&1; then
        unavailable "Azure: no se pudo obtener azurite, omitido" || return 1
        return 0
    fi
    npx --yes --package="$AZURITE_PACKAGE" azurite-blob \
        --blobHost 127.0.0.1 --blobPort "$AZURE_PORT" \
        --location "$STATE/azurite" --silent > "$STATE/azurite.log" 2>&1 &
    echo $! > "$STATE/azurite.pid"
    wait_for "Azurite" "http://127.0.0.1:$AZURE_PORT/devstoreaccount1?comp=list" || return 1
    python3 "$ROOT/seed_azure.py"
    echo "  Azure sembrado"
}

start_gcs() {
    if ! have_docker; then
        unavailable "GCS: sin docker, omitido" || return 1
        return 0
    fi
    docker rm -f karu-gcs >/dev/null 2>&1 || true
    # fake-gcs-server maps /data/<bucket>/<object> to stored objects.
    rm -rf "$STATE/gcs" && mkdir -p "$STATE/gcs/karu"
    python3 "$ROOT/fixture.py" > "$STATE/gcs/karu/fixture.bin"
    docker run -d --name karu-gcs \
        -p "127.0.0.1:$GCS_PORT:4443" \
        -v "$STATE/gcs:/data" \
        "$FAKE_GCS_IMAGE" \
        -scheme http -port 4443 \
        -external-url "http://127.0.0.1:$GCS_PORT" \
        -public-host "127.0.0.1:$GCS_PORT" >/dev/null
    wait_for "fake-gcs-server" "http://127.0.0.1:$GCS_PORT/storage/v1/b" || return 1
    curl -fsS --range 0-0 "http://127.0.0.1:$GCS_PORT/karu/fixture.bin" >/dev/null || return 1
    echo "  GCS sembrado"
}

case "$WHICH" in
    all)
        if [ "${KARU_TEST_REQUIRE_ALL_EMULATORS:-0}" = "1" ]; then
            start_s3
            start_azure
            start_gcs
        else
            start_s3 || true
            start_azure || true
            start_gcs || true
        fi
        ;;
    s3)    start_s3 ;;
    azure) start_azure ;;
    gcs)   start_gcs ;;
    *)     echo "uso: $0 [all|s3|azure|gcs]" >&2; exit 2 ;;
esac
