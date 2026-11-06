#!/bin/sh
# Benchmarks the Zig binding on matrix3.proto (the four standard tiers from
# BENCHMARKS.md). The pinned descriptor set in bindings/c/tests/data is reused,
# so no protoc step is needed; a path argument overrides it.
#
# Baseline: Arwalk/zig-protobuf (generated code) via
# `zig build -Dbaseline=true bench-baseline`. The package tarball is pinned
# in build.zig.zon and fetched on first run (then cached); generation runs
# the system protoc against their plugin, so no protoc download is involved.
#
#   ./bench.sh [matrix3.desc]             # pbwire column only, offline-safe
#   ./bench.sh --baseline [matrix3.desc]  # full comparison (needs network once)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
mode=pbwire
if [ "${1:-}" = "--baseline" ]; then mode=baseline; shift; fi
desc=${1:-$here/../../c/tests/data/matrix3.desc}

if ! command -v zig >/dev/null 2>&1; then
    echo "bench.sh: zig not found" >&2
    exit 1
fi

cd "$here/.."
if [ "$mode" = "baseline" ]; then
    exec zig build -Doptimize=ReleaseFast -Dbaseline=true bench-baseline -- "$desc"
fi
exec zig build -Doptimize=ReleaseFast bench -- "$desc"
