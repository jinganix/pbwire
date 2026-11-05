#!/bin/sh
# Builds the R extension: the core archive is linked in statically, so the
# binding needs no library path at run time. Needs R plus a C compiler.
#
#   ./build.sh   (from bindings/r; also builds ../../build/libpbwire.a)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

make -C "$root" build/libpbwire.a >/dev/null

if ! command -v R >/dev/null 2>&1; then
    echo "pbwire: R is not installed; cannot build the extension" >&2
    exit 1
fi

# R CMD SHLIB names the module after the source file, and the init symbol
# must be R_init_<module>: the source is copied as pbwire.c so the output
# is pbwire.so exposing R_init_pbwire. PKG_* vars are the documented
# SHLIB hook for extra flags (no src/Makevars edit needed for dev builds).
tmp="$here/.build-shlib"
rm -rf "$tmp"
mkdir -p "$tmp"
cp "$here/runtime/src/pbwire_r.c" "$tmp/pbwire.c"
(cd "$tmp" && \
    PKG_CPPFLAGS="-I$root/include" \
    PKG_LIBS="$root/build/libpbwire.a" \
    R CMD SHLIB pbwire.c)
cp "$tmp/pbwire.so" "$here/pbwire.so"
rm -rf "$tmp"
echo "built $here/pbwire.so"
