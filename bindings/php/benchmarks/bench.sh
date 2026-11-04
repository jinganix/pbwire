#!/bin/sh
# Benchmarks the PHP binding against the official google/protobuf PHP runtime and
# generated code on the same matrix3.proto (the four standard tiers from
# BENCHMARKS.md).
#
# The official side needs the protobuf source tree for its php/src runtime; this
# uses the cached checkout that `make conformance-setup` clones (or
# PBWIRE_PROTOBUF_SRC). No composer, no PEAR: a plain SPL autoloader loads both
# the generated classes and that runtime. When the protobuf C extension is
# installed the first run times it (`protobuf-php-c` rows) and a second
# `php -n` run captures the pure-PHP baseline (`protobuf-php` rows); without
# the extension there is only the pure-PHP run.
#
#   ./bench.sh          generate, build, run
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
src=${PBWIRE_PROTOBUF_SRC:-$root/.tmp/protobuf-v36.2/php/src}
proto=$root/bindings/c/tests/data/matrix3.proto
gen=$here/gen

if [ ! -f "$src/Google/Protobuf/Internal/Message.php" ]; then
    echo "bench.sh: no protobuf PHP runtime at $src (run 'make conformance-setup'," \
        "or set PBWIRE_PROTOBUF_SRC)" >&2
    exit 1
fi

mkdir -p "$gen"
protoc -I"$(dirname "$proto")" --php_out="$gen" \
    --descriptor_set_out="$gen/matrix3.desc" --include_imports "$proto"

make -C "$root" shared >/dev/null

php "$here/bench.php" "$gen/matrix3.desc"

# When the C extension is present the run above timed it (`protobuf-php-c`
# rows); a second `php -n` run captures the pure-PHP baseline too.
if php -m 2>/dev/null | grep -qi '^protobuf$'; then
    echo
    echo '--- pure-PHP baseline (extensions disabled) ---'
    echo
    php -n "$here/bench.php" "$gen/matrix3.desc"
fi
