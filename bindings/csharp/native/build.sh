#!/bin/sh
# Build the C# binding's native side: the C layout reader
# (native/pbw_cs_shim.c) plus the C binding's `pbw_struct` codec and the core,
# into build/libpbwire_cs_shim.{dylib,so}. The managed runtime loads it beside
# libpbwire (PBWIRE_LIB, else build/): the tree arms keep the shared core, the
# `shim` arm hands one flat image to this library (one P/Invoke per op).
#
#   bindings/csharp/native/build.sh     build the shim library
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
cc=${CC:-clang}

if [ ! -f "$root/build/libpbwire.a" ]; then
    make -C "$root" build/libpbwire.a
fi

case $(uname -s) in
Darwin) lib="libpbwire_cs_shim.dylib"; link="-dynamiclib" ;;
*) lib="libpbwire_cs_shim.so"; link="-shared" ;;
esac

mkdir -p "$root/build"
# PBW_EXTRA_CFLAGS appends to the build (sanitizers pass -fsanitize=address).
# shellcheck disable=SC2086 # $link is a flag list
$cc -std=c99 -O2 -fPIC -Wall -Wextra \
    -I"$root/include" -I"$root/bindings/c/runtime" \
    ${PBW_EXTRA_CFLAGS:-} \
    $link -o "$root/build/$lib" \
    "$here/pbw_cs_shim.c" \
    "$root/bindings/c/runtime/pbw_struct.c" \
    "$root/build/libpbwire.a"

echo "built $root/build/$lib"
