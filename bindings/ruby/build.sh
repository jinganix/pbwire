#!/bin/sh
# Builds the C extension for the Ruby that will load it: the shared library in
# build/ is linked statically into lib/pbwire_ext.<DLEXT>, so the binding needs
# no library path at run time. $RUBY picks the interpreter (default: ruby).
#
#   ./build.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
ruby=${RUBY:-ruby}

cfg() {
    "$ruby" -rrbconfig -e 'print RbConfig::CONFIG[ARGV[0]]' "$1"
}

make -C "$root" all >/dev/null

cc=$(cfg CC)
ldshared=$(cfg LDSHARED)
dlext=$(cfg DLEXT)
inc1=$(cfg rubyhdrdir)
inc2=$(cfg rubyarchhdrdir)
case $(cfg host_os) in
darwin*) extra_ldflags='-Wl,-undefined,dynamic_lookup' ;;
*) extra_ldflags='' ;;
esac

src=$here/ext/pbwire_ext/pbwire_ext.c
obj=$here/lib/pbwire_ext.o
out=$here/lib/pbwire_ext.$dlext

$cc -O2 -fPIC -Wall -Wextra -Wno-missing-field-initializers \
    -Wno-unused-parameter -I"$root/include" -I"$inc1" -I"$inc2" \
    -c -o "$obj" "$src"
# shellcheck disable=SC2086
$ldshared $extra_ldflags -o "$out" "$obj" "$root/build/libpbwire.a"
rm -f "$obj"
echo "built $out"
