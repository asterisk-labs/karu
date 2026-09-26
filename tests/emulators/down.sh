#!/bin/sh
# Stop everything tests/emulators/up.sh started.
set -u
STATE="${KARU_EMULATOR_STATE:-${TMPDIR:-/tmp}/karu-emulators}"
docker rm -f karu-minio karu-gcs >/dev/null 2>&1 || true
for service in minio azurite; do
    if [ -f "$STATE/$service.pid" ]; then
        kill "$(cat "$STATE/$service.pid")" >/dev/null 2>&1 || true
        rm -f "$STATE/$service.pid"
    fi
done
pkill -f "azurite-blob --blobHost 127.0.0.1" >/dev/null 2>&1 || true
rm -rf "$STATE"
echo "emuladores detenidos"
