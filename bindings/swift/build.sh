#!/bin/sh
# Swift binding driver: self-check + example (mirrors bindings/objc/build.sh).
# From bindings/swift:
#
#   ./build.sh          self-check + example
#   ./build.sh test     self-check only
#   ./build.sh example  example only
#
# Builds the C core first (`make` at the repo root writes the git-ignored
# build/libpbwire.a this package links), regenerates the fixtures when protoc
# is available, then runs plain `swift test` / `swift run` — no environment
# setup, no dependencies beyond the Swift toolchain.
set -eu

cd "$(dirname "$0")"
ROOT="../.."

if [ ! -f "$ROOT/build/libpbwire.a" ]; then
    make -C "$ROOT"
fi

if command -v protoc >/dev/null 2>&1; then
    protoc -Itests/data \
        --descriptor_set_out=tests/data/probe.desc tests/data/probe.proto
    protoc -I"$ROOT/bindings/c/tests/data" \
        --descriptor_set_out=tests/data/matrix3.desc matrix3.proto
fi

# `swift test` on this toolchain only discovers the XCTest suite with an
# explicit filter (otherwise the new runner reports 0 tests), so filter by
# the suite name; it runs all 8.
case "${1:-all}" in
    test) swift test --filter PbwireTests ;;
    example) swift run Roundtrip ;;
    all) swift test && swift run Roundtrip ;;
    *) echo "usage: $0 [test|example|all]" >&2; exit 2 ;;
esac
