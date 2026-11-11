#!/usr/bin/env bash
# Dynamic-symbol gate: the shared library must export exactly the functions
# include/pbwire.h declares.
#
# The core is built with -fvisibility=hidden and the public header wraps its
# declarations in `#pragma GCC visibility push(default)`, so the ABI surface is
# the header: an internal helper (src/internal.h) cannot leak into the dynamic
# symbol table, and a public function cannot silently disappear from it.
# Usage: scripts/check-symbols.sh [path-to-shared-library]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

shlib="${1:-}"
if [ -z "$shlib" ]; then
    make -s shared
    case "$(uname -s)" in
    Darwin) shlib=build/libpbwire.1.dylib ;;
    *) shlib=build/libpbwire.so.1 ;;
    esac
fi
[ -f "$shlib" ] || { echo "check-symbols: $shlib not found" >&2; exit 1; }

declared=$(grep -o 'pbw_[a-z_0-9]*(' include/pbwire.h | tr -d '(' | sort -u)
# -D reads .dynsym (ELF); Mach-O has no -D, and its symbols carry a leading "_".
if nm -D --defined-only "$shlib" > /dev/null 2>&1; then
    exported=$(nm -D --defined-only "$shlib" | awk '$2 ~ /[TtWw]/ { print $3 }')
else
    exported=$(nm -g --defined-only "$shlib" | awk '$2 ~ /[TtWw]/ { print $3 }')
fi
exported=$(printf '%s\n' "$exported" | sed 's/^_/ /; s/^ //' | grep '^pbw_' | sort -u || true)

status=0
for sym in $exported; do
    if ! printf '%s\n' "$declared" | grep -qx "$sym"; then
        echo "check-symbols: $sym is exported but not declared in include/pbwire.h"
        status=1
    fi
done
for sym in $declared; do
    if ! printf '%s\n' "$exported" | grep -qx "$sym"; then
        echo "check-symbols: $sym is declared but not exported (missing -fvisibility=hidden?)"
        status=1
    fi
done

if [ "$status" -ne 0 ]; then
    exit 1
fi
echo "check-symbols: $(printf '%s\n' "$exported" | grep -c .) exports match include/pbwire.h"
