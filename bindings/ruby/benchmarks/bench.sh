#!/bin/sh
# Benchmarks the Ruby binding against the official google-protobuf gem on the
# same matrix3.proto (the four standard tiers from BENCHMARKS.md).
#
# Needs protoc, a C compiler and the gem (`gem install google-protobuf`); the
# binding itself stays gem-free. RUBY selects the interpreter (default: ruby).
#
#   ./bench.sh          generate, build, run
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
ruby=${RUBY:-ruby}
proto=$root/bindings/c/tests/data/matrix3.proto
gen=$here/gen

if ! "$ruby" -e 'require "google/protobuf"' 2>/dev/null; then
    echo "bench.sh: $ruby cannot load google/protobuf; run 'gem install google-protobuf'" >&2
    exit 1
fi

mkdir -p "$gen"
rm -f "$gen/matrix3_pb.rb"
protoc -I"$(dirname "$proto")" --ruby_out="$gen" \
    --descriptor_set_out="$gen/matrix3.desc" --include_imports "$proto"

RUBY=$ruby sh "$here/../build.sh" >/dev/null
exec "$ruby" "$here/bench.rb"
