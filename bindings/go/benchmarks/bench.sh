#!/bin/sh
# Benchmarks the Go binding against the official protobuf-go generated code
# on the same matrix3.proto (the four standard tiers from BENCHMARKS.md).
#
# Needs Go, protoc and protoc-gen-go; the binding itself stays
# generator-free (it loads a FileDescriptorSet at run time). The C core must
# be built first (`make` at the repo root): the dynamic package links
# build/libpbwire.a over cgo.
#
#   ./bench.sh          generate, build, run
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
proto=$root/bindings/c/tests/data/matrix3.proto
gen=$here/gen

for tool in go protoc protoc-gen-go; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "bench.sh: $tool not on PATH" >&2
        exit 1
    fi
done
if [ ! -f "$root/build/libpbwire.a" ]; then
    echo "bench.sh: $root/build/libpbwire.a is missing; run 'make' at the repo root first" >&2
    exit 1
fi

mkdir -p "$gen"
rm -f "$gen/matrix3.pb.go" "$gen/matrix3.desc"
protoc -I"$(dirname "$proto")" \
    --go_out="$gen" --go_opt=Mmatrix3.proto=pbwire/benchmarks/gen,paths=source_relative \
    --descriptor_set_out="$gen/matrix3.desc" --include_imports "$proto"

cd "$here/.." && exec go test -tags bench -run TestMatrix3Bench -v ./benchmarks/
