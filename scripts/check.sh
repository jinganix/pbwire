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
  grep -qw "$b" bindings/ruby/ext/pbwire_ext/extconf.rb || {
    echo "check: extconf.rb core list missing $b" >&2
    exit 1
  }
done

echo "check: OK"
