#!/bin/sh
# Full-version gate: generic pure-Java codec vs protobuf-java generated code
# vs DynamicMessage (matrix3, four tiers) + proto2/oneof/group/extension/
# unknown-field correctness gates. Needs JDK 17+, protoc and curl.
# No libpbwire involved on purpose: this measures the no-C path.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
version=${PROTOBUF_JAVA_VERSION:-4.36.2}
jar=${PROTOBUF_JAVA_JAR:-$here/../benchmarks/deps/protobuf-java-$version.jar}
gen=$here/gen
out=$here/out-full

matrix3=$root/bindings/c/tests/data/matrix3.proto
matrix2=$root/bindings/c/tests/data/matrix2.proto

if [ ! -f "$jar" ]; then
    mkdir -p "$(dirname "$jar")"
    echo "fetching protobuf-java $version"
    curl -fsSL -o "$jar" \
        "https://repo1.maven.org/maven2/com/google/protobuf/protobuf-java/$version/protobuf-java-$version.jar"
fi

mkdir -p "$gen"
rm -rf "${gen:?}/structtest"
protoc -I"$(dirname "$matrix3")" --java_out="$gen" \
    --descriptor_set_out="$gen/matrix3.desc" --include_imports "$matrix3"
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
javac -d "$out" -cp "$jar" "$here"/Wire.java "$here"/GenericCodec.java \
    "$here"/GenericBench.java "$gen"/structtest/*.java

exec java -cp "$out:$jar" mvp.GenericBench "$gen/matrix3.desc" \
    "$gen/matrix2.desc" "$(dirname "$matrix2")/matrix2.bin" \
    "$gen/edge.desc" "$root/tests/data/edge.bin" "$gen/desc_proto2.desc"
