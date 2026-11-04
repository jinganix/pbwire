#!/bin/sh
# Benchmarks the Rust binding against rust-protobuf's generated code and prost
# (the fastest widely used Rust runtime) on the same matrix3.proto (the four
# standard tiers from BENCHMARKS.md).
#
# Needs protoc, a cargo toolchain and `cargo install protobuf-codegen` (which
# provides the protoc-gen-rs plugin and the protobuf runtime the generated code
# needs). The prost baseline is generated at build time by build.rs (needs
# crates.io for the prost/prost-build crates).
#
#   ./bench.sh          generate, build, run
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
proto=$root/bindings/c/tests/data/matrix3.proto
gen=$here/gen

if ! command -v protoc-gen-rs >/dev/null 2>&1; then
    echo "bench.sh: protoc-gen-rs not found (cargo install protobuf-codegen)" >&2
    exit 1
fi

mkdir -p "$gen"
protoc -I"$(dirname "$proto")" --rs_out="$gen" \
    --descriptor_set_out="$gen/matrix3.desc" --include_imports "$proto"

make -C "$root" all >/dev/null

exec cargo run --release --quiet --manifest-path "$here/Cargo.toml" -- "$gen/matrix3.desc"
