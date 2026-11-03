#!/bin/sh
# Benchmarks the Java binding against the official protobuf-java generated
# code on the same matrix3.proto (the four standard tiers from BENCHMARKS.md).
#
# Needs JDK 22+, protoc and curl; downloads protobuf-java (Maven Central) into
# benchmarks/deps on first run. Override the jar with PROTOBUF_JAVA_JAR.
#
#   ./bench.sh          build and run
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
version=${PROTOBUF_JAVA_VERSION:-4.36.2}
jar=${PROTOBUF_JAVA_JAR:-$here/deps/protobuf-java-$version.jar}
gen=$here/gen
out=$here/out

proto=$root/bindings/c/tests/data/matrix3.proto

if [ ! -f "$jar" ]; then
    mkdir -p "$here/deps"
    echo "fetching protobuf-java $version"
    curl -fsSL -o "$jar" \
        "https://repo1.maven.org/maven2/com/google/protobuf/protobuf-java/$version/protobuf-java-$version.jar"
fi

mkdir -p "$gen"
rm -rf "${gen:?}/structtest"
protoc -I"$(dirname "$proto")" --java_out="$gen" \
    --descriptor_set_out="$gen/matrix3.desc" --include_imports "$proto"

make -C "$root" shared >/dev/null
case "$(uname -s)" in
Darwin) lib=$root/build/libpbwire.dylib ;;
*) lib=$root/build/libpbwire.so ;;
esac

rm -rf "$out"
mkdir -p "$out"
javac -d "$out" -cp "$jar" "$root"/bindings/java/runtime/*.java "$here/Bench.java" \
    "$gen"/structtest/*.java

exec java --enable-native-access=ALL-UNNAMED -Dpbwire.library="$lib" \
    -cp "$out:$jar" Bench "$gen/matrix3.desc"
