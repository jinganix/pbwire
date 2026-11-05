#!/bin/sh
# Build and check the Node.js binding: compile the N-API addon against
# build/libpbwire.a (building it first when missing), then run the self-check.
# clang on purpose: no node-gyp, no npm, no dependencies.
#
#   bindings/node/build.sh          build the addon, then run the tests
#   bindings/node/build.sh build    build only
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cc=${CC:-clang}

if [ ! -f "$root/build/libpbwire.a" ]; then
    # The default make goal is the shared library; the addon links the static
    # archive, so name it explicitly.
    make -C "$root" build/libpbwire.a
fi

# Node ships its headers next to the binary: <prefix>/bin/node and
# <prefix>/include/node. Override with NODE_INCLUDE if a packaging split them.
: "${NODE_INCLUDE:=$(node -p 'require("path").join(require("path").dirname(require("path").dirname(process.execPath)), "include", "node")')}"
if [ ! -f "$NODE_INCLUDE/node_api.h" ]; then
    echo "build.sh: node_api.h not found in $NODE_INCLUDE (set NODE_INCLUDE)" >&2
    exit 1
fi

# A .node module is a loadable module, not a library: resolve the node symbols
# at load time on macOS, at link time from libc elsewhere.
case $(uname -s) in
Darwin) link="-dynamiclib -undefined dynamic_lookup" ;;
*) link="-shared" ;;
esac

mkdir -p "$here/build"
# shellcheck disable=SC2086 # $link is a flag list
$cc -std=c99 -O2 -fPIC -Wall -Wextra \
    -I"$NODE_INCLUDE" -I"$root/include" \
    $link -o "$here/build/pbwire_node.node" \
    "$here/runtime/addon.c" "$root/build/libpbwire.a"

echo "built $here/build/pbwire_node.node"

if [ "${1:-all}" = build ]; then
    exit 0
fi

# --expose-gc so the finalizer test can force the collections it checks.
exec node --expose-gc --test "$here/test/test_pbwire.js" "$here/tests/typed_test.js" "$here/tests/compat_test.js"
