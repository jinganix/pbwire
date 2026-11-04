#!/bin/sh
# Builds the Lua C module for the interpreter that will load it: the core
# archive is linked in statically, so the binding needs no library path at
# run time. LUA picks the interpreter, LUA_INCDIR overrides its headers.
#
#   ./build.sh
#   LUA=luajit LUA_INCDIR=/usr/include/luajit-2.1 ./build.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
lua=${LUA:-lua}

if [ -n "${LUA_INCDIR:-}" ]; then
    inc="$LUA_INCDIR"
elif [ -d /opt/homebrew/include/lua ]; then
    inc=/opt/homebrew/include/lua
else
    inc=$(pkg-config --cflags-only-I lua 2>/dev/null | sed 's/-I//' | head -n 1)
    if [ -z "$inc" ]; then
        # Last resort: the interpreter's own include dir next to the binary.
        prefix=$(cd "$(dirname "$(command -v "$lua")")/.." && pwd)
        inc=$prefix/include
    fi
fi

make -C "$root" all >/dev/null

case "$(uname -s)" in
Darwin) out=$here/pbwire.so ;;
*) out=$here/pbwire.so ;;
esac

cc=${CC:-cc}
case "$(uname -s)" in
Darwin) extra_ldflags='-Wl,-undefined,dynamic_lookup' ;;
*) extra_ldflags='' ;;
esac
$cc -O2 -fPIC -std=c99 -Wall -Wextra -Wno-unused-parameter \
    -I"$root/include" -I"$inc" \
    -c -o "$here/pbwire.o" "$here/runtime/pbwire.c"
# shellcheck disable=SC2086
$cc -shared $extra_ldflags -o "$out" "$here/pbwire.o" "$root/build/libpbwire.a"
rm -f "$here/pbwire.o"
echo "built $out (lua: $lua, headers: $inc)"
