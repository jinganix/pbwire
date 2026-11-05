#!/bin/sh
# Benchmarks the Zig binding on matrix3.proto (the four standard tiers from
# BENCHMARKS.md). The pinned descriptor set in bindings/c/tests/data is reused,
# so no protoc step is needed; a path argument overrides it.
#
# Baseline note: the official comparison point for Zig is Arwalk/zig-protobuf
# (generated code). It is not wired in yet: it needs network access (a Zig
# package fetch plus its protoc plugin) that this checkout does not assume, so
# this script reports the pbwire-zig column only. Plugging the baseline in
# means generating its module from the same matrix3.proto and timing the same
# fill/encode/decode ops; the WANT sizes and the harness block format already
# match BENCHMARKS.md so the numbers will drop straight into its table.
#
#   ./bench.sh [matrix3.desc]
set -eu

here=$(cd "$(dirname "$0")" && pwd)
desc=${1:-$here/../../c/tests/data/matrix3.desc}

if ! command -v zig >/dev/null 2>&1; then
    echo "bench.sh: zig not found" >&2
    exit 1
fi

cd "$here/.."
exec zig build -Doptimize=ReleaseFast bench -- "$desc"
