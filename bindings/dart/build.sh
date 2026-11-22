#!/bin/sh
# Build the Dart binding's native side: the C layout reader
# (native/pbw_dart_shim.c) plus the C binding's `pbw_struct` codec and the
# core, into build/libpbwire_dart.{dylib,so}. The Dart runtime dlopens it
# beside libpbwire: the tree arms keep the shared core, the `shim` arm walks a
# `dart:ffi` Struct in place through this library (one call per op).
#
#   bindings/dart/build.sh          build the shim library
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cc=${CC:-clang}

if [ ! -f "$root/build/libpbwire.a" ]; then
    make -C "$root" build/libpbwire.a
fi

case $(uname -s) in
Darwin) lib="libpbwire_dart.dylib"; link="-dynamiclib" ;;
*) lib="libpbwire_dart.so"; link="-shared" ;;
esac

mkdir -p "$here/build"
# PBW_EXTRA_CFLAGS appends to the build (sanitizers pass -fsanitize=address).
# shellcheck disable=SC2086 # $link is a flag list
$cc -std=c99 -O2 -fPIC -Wall -Wextra \
    -I"$root/include" -I"$root/bindings/c/runtime" \
    ${PBW_EXTRA_CFLAGS:-} \
    $link -o "$here/build/$lib" \
    "$here/native/pbw_dart_shim.c" \
    "$root/bindings/c/runtime/pbw_struct.c" \
    "$root/build/libpbwire.a"

echo "built $here/build/$lib"
