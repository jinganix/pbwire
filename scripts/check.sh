#!/usr/bin/env bash
# Pre-commit / pre-push gate, invoked by .githooks/pre-commit and .githooks/pre-push.
# Builds the library and runs the assert-based self-check.
# Emergency bypass: SKIP_CHECKS=1 ./scripts/check.sh (only with a real reason).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [ "${SKIP_CHECKS:-0}" = "1" ]; then
  echo "check: SKIP_CHECKS=1 -- skipping gate"
  exit 0
fi

echo "== versions =="
./scripts/sync-versions.sh --check

echo "== llms.txt sync =="
./scripts/check-llms.sh

echo "== build + test =="
make -s test

echo "== dynamic symbols =="
./scripts/check-symbols.sh

echo "== erlang vendored core =="
make -s -C bindings/erlang/c_src core-sync-check

echo "== binding core file lists =="
for f in src/*.c; do
  b=${f#src/}
  b=${b%.c}
  # PHP compiles symlinks, so the link target is the invariant (a real
  # file in its place would silently freeze the core it was copied from).
  [ "$(readlink "bindings/php/ext/core_$b.c")" = "../../../$f" ] || {
    echo "check: bindings/php/ext/core_$b.c must symlink to ../../../$f" >&2
    exit 1
  }
  grep -qw "core_$b\.c" bindings/php/ext/config.m4 || {
    echo "check: config.m4 core list missing core_$b.c" >&2
    exit 1
  }
  # The gem packs real files (symlinks and parent paths are rejected at
  # install), so the copy must match the repo root, not merely exist.
  cmp -s "$f" "bindings/ruby/ext/pbwire_ext/core_$b.c" || {
    echo "check: bindings/ruby/ext/pbwire_ext/core_$b.c differs from $f" >&2
    exit 1
  }
  grep "^core = " bindings/ruby/ext/pbwire_ext/extconf.rb | grep -qw "$b" || {
    echo "check: extconf.rb core list missing $b" >&2
    exit 1
  }
done
# Reverse direction: a stale vendored file with no repo source must go.
for f in bindings/ruby/ext/pbwire_ext/core_*.c bindings/php/ext/core_*.c; do
  b=${f##*/core_}
  [ -e "src/$b" ] || {
    echo "check: $f has no repo-root source; delete it" >&2
    exit 1
  }
done
for f in include/pbwire.h src/internal.h; do
  cmp -s "$f" "bindings/ruby/ext/pbwire_ext/$(basename "$f")" || {
    echo "check: bindings/ruby/ext/pbwire_ext/$(basename "$f") differs from $f" >&2
    exit 1
  }
done
# The Zig binding and CMake keep their own core file lists (no globs),
# so a new src/*.c must be added in three more places.
for f in src/*.c; do
  b=$(basename "$f")
  # The Makefile's SRC list is the one place `make` links from: a missing
  # entry is an undefined symbol for consumers of libpbwire.a.
  grep -q "src/$b" Makefile || {
    echo "check: Makefile SRC list missing src/$b" >&2
    exit 1
  }
  grep -q "\"$b\"" bindings/zig/build.zig || {
    echo "check: bindings/zig/build.zig core list missing $b" >&2
    exit 1
  }
  grep -q "src/$b" CMakeLists.txt || {
    echo "check: CMakeLists.txt core list missing src/$b" >&2
    exit 1
  }
  # The Python sdist stages explicit core copies next to setup.py, so its
  # CORE list must name every core file too.
  grep -q "\"$b\"" bindings/python/setup.py || {
    echo "check: bindings/python/setup.py CORE missing $b" >&2
    exit 1
  }
  # The Lua rock builds in-tree with explicit paths; a missing entry is a
  # link-time undefined symbol (e.g. wkt.c for pbw_any_*).
  grep -q "\"src/$b\"" bindings/lua/pbwire-*.rockspec || {
    echo "check: bindings/lua/pbwire-*.rockspec sources missing src/$b" >&2
    exit 1
  }
done

echo "== formatting =="
if command -v clang-format >/dev/null 2>&1; then
  make -s format-check
else
  echo "check: WARN: clang-format not found, skipping format-check" >&2
fi

echo "check: OK"
