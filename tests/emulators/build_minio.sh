#!/bin/sh
# Build MinIO and mc for the S3 tests. Requires Go 1.24+.
# Usage: tests/emulators/build_minio.sh [directory]
set -eu

DEST="${1:-${KARU_MINIO_BIN_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/karu-minio}}"
# MinIO RELEASE.2025-04-22T22-12-26Z; mc RELEASE.2024-11-21T17-21-54Z.
MINIO_COMMIT="f19c534b9f457773dcd043d977433e1a71525c3b"
MC_COMMIT="ec185ff65d64f1e57a10a1870c5149f3a2e31d52"

mkdir -p "$DEST"
DEST=$(cd "$DEST" && pwd)
echo "compilando MinIO y mc en $DEST (unos minutos la primera vez)"
GOBIN="$DEST" CGO_ENABLED=0 go install "github.com/minio/minio@$MINIO_COMMIT"
GOBIN="$DEST" CGO_ENABLED=0 go install "github.com/minio/mc@$MC_COMMIT"
"$DEST/minio" --version | head -1
