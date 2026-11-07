#!/usr/bin/env bash
# Cross-architecture gate: build the core and its dependency-free suites for a
# foreign target and run them under qemu-user. CI uses this for big-endian
# (s390x) coverage, which no runner provides natively; 32-bit runs natively on
# an x86_64 runner (see ci.yml) and needs no emulation.
#
#   scripts/check-ports.sh                    # s390x (big-endian)
#   CROSS=... RUN=... scripts/check-ports.sh  # any cross toolchain + qemu
#
# Every suite here is static and file-light on purpose: test_struct reads the
# pinned fixtures from the repo root, which qemu-user can do because it runs
# the binary in-process.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

CROSS=${CROSS:-s390x-linux-gnu-gcc}
RUN=${RUN:-qemu-s390x-static}
CFLAGS="-O2 -std=c99 -Wall -Wextra -Werror -pedantic -static"
OUT=build/port
mkdir -p "$OUT"

echo "port: $CROSS $($CROSS --version | head -n 1)"

build() { # <out> <extra flags...>
    local out=$1
    shift
    # shellcheck disable=SC2086
    "$CROSS" $CFLAGS -Iinclude -Isrc "$@" -o "$OUT/$out"
}

run() {
    echo "== $1"
    "$RUN" "$OUT/$1"
}

build test_pbwire tests/test_pbwire.c src/*.c
build test_malformed tests/test_malformed.c src/*.c
build test_descriptor tests/test_descriptor.c src/*.c
build test_struct -Ibindings/c/runtime -Ibindings/c/tests \
    -Ibindings/c/examples bindings/c/tests/test_struct.c \
    bindings/c/runtime/pbw_struct.c src/*.c
build test_embedded -Ibindings/embedded/runtime \
    bindings/embedded/tests/test_embedded.c bindings/embedded/runtime/pbwe.c \
    src/*.c
build example_gateway bindings/c/examples/gateway.c src/*.c

run test_pbwire
run test_malformed
run test_descriptor
run test_struct
run test_embedded
run example_gateway
echo "port: all suites passed on $(${CROSS} -dumpmachine)"
