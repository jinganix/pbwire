#!/bin/sh
# Benchmarks the Dart binding against the official protobuf Dart runtime and
# generated code on the same matrix3.proto (the four standard tiers from
# BENCHMARKS.md).
#
# Needs protoc, the Dart SDK and `dart pub global activate protoc_plugin` (which
# provides protoc-gen-dart); `dart pub get` fetches the protobuf package on the
# first run.
#
#   ./bench.sh          generate, build, run
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
proto=$root/bindings/c/tests/data/matrix3.proto
gen=$here/gen

if ! command -v protoc-gen-dart >/dev/null 2>&1; then
    echo "bench.sh: protoc-gen-dart not found (dart pub global activate protoc_plugin," \
        "and add ~/.pub-cache/bin to PATH)" >&2
    exit 1
fi

mkdir -p "$gen"
protoc -I"$(dirname "$proto")" --dart_out="$gen" \
    --descriptor_set_out="$gen/matrix3.desc" --include_imports "$proto"

make -C "$root" shared >/dev/null

cd "$here"
dart pub get >/dev/null
exec dart run bench.dart "$gen/matrix3.desc"
