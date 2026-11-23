#!/bin/sh
# Stage 6 A/B probe: two extension builds from the same tree. Each build gets
# its own temp lib/ layout (lib/pbwire.rb is require_relative'd), so the two
# .so copies never load into the same process.
#
#   bindings/ruby/pbench/ab.sh BASE.bundle CAND.bundle [tier] [op] [rounds]
set -eu

here=$(cd "$(dirname "$0")" && pwd)
bind=$(cd "$here/.." && pwd)
base=$1
cand=$2
tier=${3:-xlarge}
op=${4:-encode}
rounds=${5:-5}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

setup_root() {
    dir=$1
    ext=$2
    mkdir -p "$dir/lib" "$dir/test/gen" "$dir/test/gen_oracle" "$dir/pbench"
    cp "$bind/lib/pbwire.rb" "$dir/lib/"
    cp "$ext" "$dir/lib/pbwire_ext.bundle"
    cp "$bind/test/gen/matrix3.rb" "$bind/test/gen/matrix3_meta.rb" "$dir/test/gen/"
    cp "$bind/test/gen_oracle/matrix3_pb.rb" "$dir/test/gen_oracle/"
    cp "$here/payload.rb" "$here/b-shim.rb" "$here/ab.rb" "$dir/pbench/"
}

setup_root "$tmp/base" "$base"
setup_root "$tmp/cand" "$cand"

printf 'round  base MB/s  cand MB/s\n'
i=1
while [ "$i" -le "$rounds" ]; do
    b=$(ruby "$tmp/base/pbench/ab.rb" "$tier" "$op")
    c=$(ruby "$tmp/cand/pbench/ab.rb" "$tier" "$op")
    printf '%5s %10s %10s\n' "$i" "$b" "$c"
    i=$((i + 1))
done
