#!/bin/sh
# Classpath A/B for the pure-Java codec: runs the same Ab probe against two
# build trees in alternating rounds (so a drifting machine hits both sides
# equally) and prints the best time seen per op on each side.
#
#   benchmarks/ab.sh <base-classes-dir> <cand-classes-dir> [rounds]
#
# Build both sides from their own source trees, e.g.
#   ./build.sh && cp -R out/classes /tmp/base
#   (make the change) && ./build.sh   # out/classes is the candidate
#   benchmarks/ab.sh /tmp/base out/classes
#
# Anything under ~5% is noise unless the two sides differ in every round;
# read docs/perf-attempts.md before proposing a new attempt.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
version=${PROTOBUF_JAVA_VERSION:-4.36.2}
jar=${PROTOBUF_JAVA_JAR:-$here/deps/protobuf-java-$version.jar}
desc=$here/gen/matrix3.desc
bin=$root/bindings/c/tests/data/matrix3.bin
rounds=${3:-3}
# Fixed heap + SerialGC keeps allocation/GC noise out of the comparison.
JAVA_FLAGS=${JAVA_FLAGS:--Xms1g -Xmx1g -XX:+UseSerialGC}

base=$1
cand=$2
if [ ! -d "$base" ] || [ ! -d "$cand" ]; then
    echo "usage: ab.sh <base-classes-dir> <cand-classes-dir> [rounds]" >&2
    exit 2
fi
if [ ! -f "$jar" ]; then
    echo "missing $jar (run benchmarks/bench.sh once)" >&2
    exit 1
fi
if [ ! -f "$desc" ]; then
    mkdir -p "$(dirname "$desc")"
    protoc -I"$root/bindings/c/tests/data" \
        --descriptor_set_out="$desc" --include_imports \
        "$root/bindings/c/tests/data/matrix3.proto"
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/probe"
javac -nowarn -d "$tmp/probe" -cp "$base:$jar" "$here/Ab.java"

i=0
while [ "$i" -lt "$rounds" ]; do
    java $JAVA_FLAGS -cp "$tmp/probe:$base:$jar" Ab "$desc" "$bin" 2>/dev/null >>"$tmp/base.raw"
    java $JAVA_FLAGS -cp "$tmp/probe:$cand:$jar" Ab "$desc" "$bin" 2>/dev/null >>"$tmp/cand.raw"
    i=$((i + 1))
done

for side in base cand; do
    awk -F'\t' '{ if (!($1 in best) || $2 < best[$1]) best[$1] = $2 }
        END { for (k in best) printf "%s\t%.1f\n", k, best[k] }' \
        "$tmp/$side.raw" | sort >"$tmp/$side"
done

awk -F'\t' 'NR==FNR { b[$1] = $2; next } {
        printf "%-22s %8.1f -> %8.1f ns  %6.2fx\n", $1, b[$1], $2, b[$1] / $2
    }' "$tmp/base" "$tmp/cand"
