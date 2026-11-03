#!/bin/sh
# Benchmarks the Kotlin binding against the official Kotlin generated code on the
# same matrix3.proto (the four standard tiers from BENCHMARKS.md).
#
# Needs JDK 22+, kotlinc, protoc and curl; downloads protobuf-java and
# protobuf-kotlin (Maven Central) into benchmarks/deps on first run. Override the
# version with PROTOBUF_JAVA_VERSION.
#
#   ./bench.sh          generate, build, run
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
version=${PROTOBUF_JAVA_VERSION:-4.36.2}
java_jar=${PROTOBUF_JAVA_JAR:-$here/deps/protobuf-java-$version.jar}
kotlin_jar=${PROTOBUF_KOTLIN_JAR:-$here/deps/protobuf-kotlin-$version.jar}
gen=$here/gen
out=$here/out
proto=$root/bindings/c/tests/data/matrix3.proto

mkdir -p "$here/deps"
for artifact in protobuf-java protobuf-kotlin; do
    jar=$here/deps/$artifact-$version.jar
    if [ ! -f "$jar" ]; then
        echo "fetching $artifact $version"
        curl -fsSL -o "$jar" \
            "https://repo1.maven.org/maven2/com/google/protobuf/$artifact/$version/$artifact-$version.jar"
    fi
done

mkdir -p "$gen"
protoc -I"$(dirname "$proto")" --java_out="$gen" --kotlin_out="$gen" \
    --descriptor_set_out="$gen/matrix3.desc" --include_imports "$proto"

make -C "$root" shared >/dev/null
case "$(uname -s)" in
Darwin) lib=$root/build/libpbwire.dylib ;;
*) lib=$root/build/libpbwire.so ;;
esac

rm -rf "$out"
mkdir -p "$out"
javac -d "$out" -cp "$java_jar" "$root"/bindings/java/runtime/*.java "$gen"/structtest/*.java
# -include-runtime bundles kotlin-stdlib, so the run command needs no kotlinc dir.
kotlinc -nowarn -cp "$out:$java_jar:$kotlin_jar" -include-runtime -d "$out/bench.jar" \
    "$root"/bindings/kotlin/runtime/*.kt "$here"/Bench.kt "$gen"/structtest/*.kt

exec java --enable-native-access=ALL-UNNAMED -Dpbwire.library="$lib" \
    -cp "$out/bench.jar:$out:$java_jar:$kotlin_jar" BenchKt "$gen/matrix3.desc"
