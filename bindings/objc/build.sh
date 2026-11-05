#!/bin/sh
# Build and run the Objective-C binding (macOS: Apple clang + Foundation).
#
#   ./build.sh              build the self-check + example and run both
#   ./build.sh test         self-check only
#   ./build.sh typed        generated typed classes self-check only
#   ./build.sh example      example only
#
# The core static library comes from the repo root: `make all` builds
# build/libpbwire.a, which this links. Nothing outside bindings/objc is written.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
lib="$root/build/libpbwire.a"
out="$here/build"
cc=${CC:-clang}
flags="-fobjc-arc -O2 -Wall -Wextra -I$root/include -I$here"
runtime="$here/PBWSchema.m $here/PBWMessage.m"

if [ ! -f "$lib" ]; then
    echo "build.sh: $lib is missing, running 'make all' in $root"
    (cd "$root" && make all)
fi

mkdir -p "$out"

build() { # build <output> <source>
    # shellcheck disable=SC2086 # flags and runtime are word lists on purpose
    $cc $flags -o "$out/$1" $runtime "$2" "$lib" -framework Foundation -lpthread
}

run_tests() {
    build test_objc "$here/tests/test_objc.m"
    "$out/test_objc"
}

run_typed() {
    # The T4/T5 oracle is the official protobuf ObjC runtime plus
    # protoc --objc_out for the three fixtures, like benchmarks/bench.sh:
    # the cached checkout from `make conformance-setup` (or
    # PBWIRE_PROTOBUF_SRC) provides both the runtime and the pin (v36.2).
    src=${PBWIRE_PROTOBUF_SRC:-$root/.tmp/protobuf-v36.2}
    if [ ! -f "$src/objectivec/GPBProtocolBuffers.m" ]; then
        echo "build.sh: no protobuf source at $src (run 'make conformance-setup'," >&2
        echo "or set PBWIRE_PROTOBUF_SRC)" >&2
        exit 1
    fi
    gen=$out/gen_oracle
    mkdir -p "$gen"
    protoc -I"$root/bindings/c/tests/data" --objc_out="$gen" \
        "$root/bindings/c/tests/data/matrix3.proto"
    protoc -I"$root/bindings/typed/testdata" --objc_out="$gen" \
        "$root/bindings/typed/testdata/oneof1.proto"
    protoc -I"$root/examples" --objc_out="$gen" "$root/examples/addressbook.proto"
    if [ ! -f "$gen/GPBProtocolBuffers.o" ]; then
        $cc -O2 -c "$src/objectivec/GPBProtocolBuffers.m" \
            -I"$src/objectivec" -o "$gen/GPBProtocolBuffers.o"
    fi
    # The generated codec and the runtime are manual-retain-release; the
    # facade and the harness are ARC. Linking the two is fine.
    for p in Matrix3 Oneof1 Addressbook; do
        $cc -O2 -c "$gen/$p.pbobjc.m" -I"$gen" -I"$src/objectivec" \
            -o "$gen/$p.pbobjc.o"
    done
    # shellcheck disable=SC2086 # flags and runtime are word lists on purpose
    $cc $flags -I"$here/tests" -I"$here/examples" -I"$gen" \
        -I"$src/objectivec" -o "$out/typed_test" $runtime \
        "$here/tests/gen/matrix3.m" "$here/tests/gen/oneof1.m" \
        "$here/examples/gen/addressbook.m" "$here/tests/typed_test.m" \
        "$gen/Matrix3.pbobjc.o" "$gen/Oneof1.pbobjc.o" \
        "$gen/Addressbook.pbobjc.o" "$gen/GPBProtocolBuffers.o" \
        "$lib" -framework Foundation -lpthread
    "$out/typed_test" "$root/bindings/c/tests/data/matrix3.bin" \
        "$root/bindings/typed/testdata/oneof1.bin" \
        "$root/examples/addressbook.bin"
}

run_example() {
    # shellcheck disable=SC2086 # flags and runtime are word lists on purpose
    $cc $flags -I"$here/examples" -o "$out/roundtrip" $runtime \
        "$here/examples/gen/addressbook.m" "$here/examples/roundtrip.m" "$lib" \
        -framework Foundation -lpthread
    "$out/roundtrip"
}

case "${1:-all}" in
all)
    run_tests
    run_typed
    run_example
    ;;
test) run_tests ;;
typed) run_typed ;;
example) run_example ;;
*)
    echo "usage: $0 [all|test|typed|example]" >&2
    exit 2
    ;;
esac
