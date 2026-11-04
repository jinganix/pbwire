#!/bin/sh
# MVP benchmark: pure-Java procedural codec vs protobuf-java generated code.
# Needs JDK 22+ (uses no FFM, plain javac/java suffice), protoc and curl.
# No libpbwire involved on purpose: this measures the no-C path.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
version=${PROTOBUF_JAVA_VERSION:-4.36.2}
jar=${PROTOBUF_JAVA_JAR:-$here/../benchmarks/deps/protobuf-java-$version.jar}
gen=$here/gen
out=$here/out

proto=$root/bindings/c/tests/data/matrix3.proto

if [ ! -f "$jar" ]; then
    mkdir -p "$(dirname "$jar")"
    echo "fetching protobuf-java $version"
    curl -fsSL -o "$jar" \
        "https://repo1.maven.org/maven2/com/google/protobuf/protobuf-java/$version/protobuf-java-$version.jar"
fi

mkdir -p "$gen"
rm -rf "${gen:?}/structtest"
protoc -I"$(dirname "$proto")" --java_out="$gen" \
    --descriptor_set_out="$gen/matrix3.desc" --include_imports "$proto"

rm -rf "$out"
mkdir -p "$out"
javac -d "$out" -cp "$jar" "$here"/*.java "$gen"/structtest/*.java

exec java -cp "$out:$jar" mvp.PureBench "$gen/matrix3.desc"
