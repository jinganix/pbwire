#!/bin/sh
# The A/B probe for the Node shim arm: two addon builds from the same tree and
# one interleaved timing loop over both, so a per-field change can be accepted
# or rejected without trusting two separate runs.
#
#   bindings/node/benchmarks/ab.sh                 base = HEAD, cand = working tree
#   AB_BASE=HEAD~1 bindings/node/benchmarks/ab.sh  any revisions
#   AB_CAND=<rev> bindings/node/benchmarks/ab.sh   candidate from a revision
#
# Both builds link the same build/libpbwire.a; only runtime/addon.c differs.
# ab.js refuses to time anything whose bytes differ from the base build.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
cc=${CC:-clang}
out=$here/build/ab

: "${AB_BASE:=HEAD}"
: "${AB_CAND:=}"

if [ ! -f "$root/build/libpbwire.a" ]; then
    make -C "$root" build/libpbwire.a
fi

: "${NODE_INCLUDE:=$(node -p 'require("path").join(require("path").dirname(require("path").dirname(process.execPath)), "include", "node")')}"
if [ ! -f "$NODE_INCLUDE/node_api.h" ]; then
    echo "ab.sh: node_api.h not found in $NODE_INCLUDE (set NODE_INCLUDE)" >&2
    exit 1
fi

case $(uname -s) in
Darwin) link="-dynamiclib -undefined dynamic_lookup" ;;
*) link="-shared" ;;
esac

mkdir -p "$out"
git -C "$root" show "$AB_BASE:bindings/node/runtime/addon.c" > "$out/base.c"
if [ -n "$AB_CAND" ]; then
    git -C "$root" show "$AB_CAND:bindings/node/runtime/addon.c" > "$out/cand.c"
    echo "ab: base $AB_BASE, cand $AB_CAND"
else
    cp "$here/../runtime/addon.c" "$out/cand.c"
    echo "ab: base $AB_BASE, cand working tree"
fi

for v in base cand; do
    # shellcheck disable=SC2086 # $link is a flag list
    $cc -std=c99 -O2 -fPIC -Wall -Wextra \
        -I"$NODE_INCLUDE" -I"$root/include" ${PBW_EXTRA_CFLAGS:-} \
        $link -o "$out/$v.node" "$out/$v.c" "$root/build/libpbwire.a"
done

exec env AB_BASE_MODULE="$out/base.node" AB_CAND_MODULE="$out/cand.node" \
    node "$here/ab.js"
