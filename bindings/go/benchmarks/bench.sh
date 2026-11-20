#!/bin/sh
# Benchmarks the thin Go model against the official protobuf-go generated code
# on the same matrix3.proto (the four standard tiers from BENCHMARKS.md).
#
# Needs Go only. The two generated packages are the committed ones under
# ../tests (pbwire structs and protobuf-go structs), so no code generator and
# no protoc run here; the thin model needs no descriptor and no C toolchain.
#
#   ./bench.sh          build, run
set -eu

here=$(cd "$(dirname "$0")" && pwd)

if ! command -v go >/dev/null 2>&1; then
    echo "bench.sh: go not on PATH" >&2
    exit 1
fi

cd "$here/.." && exec go test -count=1 -tags bench -run TestMatrix3Bench -v ./benchmarks/
