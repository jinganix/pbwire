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
    protoc -I"$ROOT/bindings/c/tests/data" \
        --descriptor_set_out=tests/data/maptest.desc maptest.proto
    cp "$ROOT/bindings/c/tests/data/matrix3.bin" tests/data/matrix3.bin
else
    # Without protoc the committed fixtures are reused as-is: warn when a
    # .proto is newer than the tests/data/*.desc the suite actually reads
    # (not the C counterparts the files are copied from).
    _warn_stale() {
        if [ ! -e "$2" ] || [ "$1" -nt "$2" ]; then
            echo "build.sh: WARN: no protoc and $1 is newer than $2; tests run against a stale schema" >&2
        fi
    }
    _warn_stale tests/data/probe.proto tests/data/probe.desc
    _warn_stale "$ROOT/bindings/c/tests/data/matrix3.proto" tests/data/matrix3.desc
    _warn_stale "$ROOT/bindings/c/tests/data/maptest.proto" tests/data/maptest.desc
    unset -f _warn_stale
fi

# `swift test` on this toolchain only discovers the XCTest suite with an
# explicit filter (otherwise the new runner reports 0 tests), so filter by
# the suite name; it runs all 13.
case "${1:-all}" in
    test) swift test --filter PbwireTests ;;
    example) swift run Roundtrip ;;
    all) swift test --filter PbwireTests && swift run Roundtrip ;;
    *) echo "usage: $0 [test|example|all]" >&2; exit 2 ;;
esac
