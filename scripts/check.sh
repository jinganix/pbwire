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
  [ -e "bindings/php/ext/core_$b.c" ] || {
    echo "check: bindings/php/ext/core_$b.c missing (symlink to ../../../$f)" >&2
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
for f in bindings/ruby/ext/pbwire_ext/core_*.c; do
  b=${f#bindings/ruby/ext/pbwire_ext/core_}
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

echo "check: OK"
