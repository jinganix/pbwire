#!/bin/sh
# Build and run the Objective-C binding (macOS: Apple clang + Foundation).
#
#   ./build.sh              build the self-check + example and run both
#   ./build.sh test         self-check only
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

run_example() {
    build roundtrip "$here/examples/roundtrip.m"
    "$out/roundtrip"
}

case "${1:-all}" in
all)
    run_tests
    run_example
    ;;
test) run_tests ;;
example) run_example ;;
*)
    echo "usage: $0 [all|test|example]" >&2
    exit 2
    ;;
esac
