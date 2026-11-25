#!/bin/sh
# Benchmarks the Objective-C binding against the official protobuf Objective-C
# runtime + generated code on the same matrix3.proto (the four standard tiers
# from BENCHMARKS.md).
#
# The official side needs the protobuf source tree for its ObjC runtime; this
# uses the cached checkout that `make conformance-setup` clones (or
# PBWIRE_PROTOBUF_SRC). One file, objectivec/GPBProtocolBuffers.m, is the
# runtime's supported single-translation-unit build.
#
#   ./bench.sh          generate, build, run
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
cc=${CC:-clang}
src=${PBWIRE_PROTOBUF_SRC:-$root/.tmp/protobuf-v36.2}
proto=$root/bindings/c/tests/data/matrix3.proto
gen=$here/gen

if [ ! -f "$src/objectivec/GPBProtocolBuffers.m" ]; then
    echo "bench.sh: no protobuf source at $src (run 'make conformance-setup'," \
        "or set PBWIRE_PROTOBUF_SRC)" >&2
    exit 1
fi

make -C "$root" all >/dev/null

mkdir -p "$gen"
if [ ! -f "$gen/GPBProtocolBuffers.o" ]; then
    "$cc" -O2 -c "$src/objectivec/GPBProtocolBuffers.m" \
        -I"$src/objectivec" -o "$gen/GPBProtocolBuffers.o"
fi

protoc -I"$(dirname "$proto")" --objc_out="$gen" \
    --descriptor_set_out="$gen/matrix3.desc" --include_imports "$proto"

# The generated codec and the runtime are manual-retain-release; the pbwire
# wrapper and the harness are ARC. Linking the two is fine.
"$cc" -O2 -c "$gen/Matrix3.pbobjc.m" -I"$gen" -I"$src/objectivec" \
    -o "$gen/Matrix3.pbobjc.o"

"$cc" -fobjc-arc -O2 -Wall -Wextra \
    -I"$root/include" -I"$here/.." -I"$gen" -I"$src/objectivec" \
    -o "$gen/bench_objc" \
    "$here/../PBWSchema.m" "$here/../PBWMessage.m" "$here/bench_objc.m" \
    "$gen/Matrix3.pbobjc.o" "$gen/GPBProtocolBuffers.o" "$root/build/libpbwire.a" \
    -framework Foundation -lpthread

exec "$gen/bench_objc" "$gen/matrix3.desc"
