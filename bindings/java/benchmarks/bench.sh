#!/bin/sh
# Benchmarks the pure-Java binding against the official protobuf-java
# generated code on the same matrix3.proto (the four standard tiers from
# BENCHMARKS.md), plus correctness gates for proto2, oneof, groups,
# extensions and unknown fields.
#
# Needs JDK 17+, protoc and curl. Downloads protobuf-java (Maven Central)
# into benchmarks/deps on first run, for the baseline only: the binding
# itself has no dependencies. Override with PROTOBUF_JAVA_JAR.
#
#   ./bench.sh          # full: 4 tiers + gates (or: make java-bench)
#   TIER=xlarge ./bench.sh   # one tier, for iteration
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
version=${PROTOBUF_JAVA_VERSION:-4.36.2}
jar=${PROTOBUF_JAVA_JAR:-$here/deps/protobuf-java-$version.jar}
gen=$here/gen
out=$here/out

proto=$root/bindings/c/tests/data/matrix3.proto
matrix2=$root/bindings/c/tests/data/matrix2.proto

if [ ! -f "$jar" ]; then
    mkdir -p "$here/deps"
    echo "fetching protobuf-java $version (baseline only)"
    curl -fsSL -o "$jar" \
        "https://repo1.maven.org/maven2/com/google/protobuf/protobuf-java/$version/protobuf-java-$version.jar"
fi

mkdir -p "$gen"
rm -rf "${gen:?}/structtest"
protoc -I"$(dirname "$proto")" --java_out="$gen" \
    --descriptor_set_out="$gen/matrix3.desc" --include_imports "$proto"
protoc -I"$(dirname "$matrix2")" \
    --descriptor_set_out="$gen/matrix2.desc" --include_imports "$matrix2"
protoc -I"$root/tests/data" \
    --descriptor_set_out="$gen/edge.desc" --include_imports \
    "$root/tests/data/edge.proto"
protoc -I"$root/tests/data" \
    --descriptor_set_out="$gen/desc_proto2.desc" --include_imports \
    "$root/tests/data/desc_proto2.proto"

rm -rf "$out"
mkdir -p "$out"
javac -d "$out" -cp "$jar" "$root"/bindings/java/runtime/*.java \
    "$here/Bench.java" "$gen"/structtest/*.java

exec java -cp "$out:$jar" Bench "$gen/matrix3.desc" \
    "$gen/matrix2.desc" "$root/bindings/c/tests/data/matrix2.bin" \
    "$gen/edge.desc" "$root/tests/data/edge.bin" "$gen/desc_proto2.desc"
