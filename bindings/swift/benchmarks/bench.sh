#!/bin/sh
# Swift binding benchmark: the four standard tiers from BENCHMARKS.md against
# apple/swift-protobuf generated code on the same matrix3.proto.
#
#   ./bench.sh          tiers + baseline (needs network once for swift-protobuf)
#   ./bench.sh tiers    pbwire-swift tiers only (no network)
#
# protoc regenerates tests/data/matrix3.desc when it is available; PROTOC_PATH
# points the baseline's build plugin at the same compiler.
set -eu

cd "$(dirname "$0")/.."
ROOT="../.."

if [ ! -f "$ROOT/build/libpbwire.a" ]; then
    make -C "$ROOT"
fi

if command -v protoc >/dev/null 2>&1; then
    protoc -I"$ROOT/bindings/c/tests/data" \
        --descriptor_set_out=tests/data/matrix3.desc matrix3.proto
    export PROTOC_PATH="${PROTOC_PATH:-$(command -v protoc)}"
fi

swift run -c release Bench

if [ "${1:-all}" = "all" ]; then
    cd benchmarks/baseline
    swift run -c release BenchBoth
fi
