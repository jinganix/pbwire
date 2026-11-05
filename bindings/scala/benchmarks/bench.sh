#!/bin/sh
# Benchmarks the Scala binding against the official Scala baseline (ScalaPB
# generated code) on the same matrix3.proto (the four standard tiers from
# BENCHMARKS.md).
#
# Needs scalac, protoc and scalapbc (the ScalaPB compiler: `cs install
# scalapb`, or override with SCALAPBC=/path/to/scalapbc). Downloads the
# ScalaPB runtime (and protobuf-java) from Maven Central into
# benchmarks/deps on first run. Override the pins with
# SCALAPB_VERSION / PROTOBUF_JAVA_VERSION.
#
#   ./bench.sh          generate, build, run
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
scalapb_version=${SCALAPB_VERSION:-0.11.18}
proto_version=${PROTOBUF_JAVA_VERSION:-4.36.2}
gen=$here/gen
out=$here/out
proto=$root/bindings/c/tests/data/matrix3.proto

mkdir -p "$here/deps"
scalapb_jar=$here/deps/scalapb-runtime_3-$scalapb_version.jar
if [ ! -f "$scalapb_jar" ]; then
    echo "fetching scalapb-runtime $scalapb_version"
    curl -fsSL -o "$scalapb_jar" \
        "https://repo1.maven.org/maven2/com/thesamet/scalapb/scalapb-runtime_3/$scalapb_version/scalapb-runtime_3-$scalapb_version.jar"
fi
proto_jar=${PROTOBUF_JAVA_JAR:-$root/bindings/java/benchmarks/deps/protobuf-java-$proto_version.jar}
lenses_jar=$here/deps/lenses_3-$scalapb_version.jar
if [ ! -f "$lenses_jar" ]; then
    echo "fetching lenses $scalapb_version"
    curl -fsSL -o "$lenses_jar" \
        "https://repo1.maven.org/maven2/com/thesamet/scalapb/lenses_3/$scalapb_version/lenses_3-$scalapb_version.jar"
fi
# scala-library jars matching the local scalac (override with SCALA3_JAR /
# SCALA_LIB_JAR when the compiler is not 3.10.0).
scala3_jar=${SCALA3_JAR:-$here/deps/scala3-library_3-3.10.0.jar}
if [ ! -f "$scala3_jar" ]; then
    echo "fetching scala3-library 3.10.0"
    curl -fsSL -o "$scala3_jar" \
        "https://repo1.maven.org/maven2/org/scala-lang/scala3-library_3/3.10.0/scala3-library_3-3.10.0.jar"
fi
scala_lib_jar=${SCALA_LIB_JAR:-$here/deps/scala-library-3.10.0.jar}
if [ ! -f "$scala_lib_jar" ]; then
    echo "fetching scala-library 3.10.0"
    curl -fsSL -o "$scala_lib_jar" \
        "https://repo1.maven.org/maven2/org/scala-lang/scala-library/3.10.0/scala-library-3.10.0.jar"
fi
if [ ! -f "$proto_jar" ]; then
    echo "fetching protobuf-java $proto_version"
    curl -fsSL -o "$proto_jar" \
        "https://repo1.maven.org/maven2/com/google/protobuf/protobuf-java/$proto_version/protobuf-java-$proto_version.jar"
fi

if ! command -v scalac >/dev/null 2>&1; then
    echo "scalac is required (scala 2.13+ or 3.x)" >&2
    exit 1
fi
if ! command -v protoc >/dev/null 2>&1; then
    echo "protoc is required" >&2
    exit 1
fi
scalapbc=${SCALAPBC:-scalapbc}
if ! command -v "$scalapbc" >/dev/null 2>&1; then
    echo "$scalapbc is required for the ScalaPB baseline:" >&2
    echo "  cs install scalapb   # or: SCALAPBC=/path/to/scalapbc ./bench.sh" >&2
    exit 1
fi

# ScalaPB baseline sources plus the descriptor set both sides load.
mkdir -p "$gen"
"$scalapbc" -I"$(dirname "$proto")" --scala_out="$gen" "$proto"
protoc -I"$(dirname "$proto")" \
    --descriptor_set_out="$gen/matrix3.desc" --include_imports "$proto"

rm -rf "$out"
mkdir -p "$out"
javac -nowarn -d "$out" "$root"/bindings/java/runtime/*.java
CP="$out:$scalapb_jar:$lenses_jar:$proto_jar:$scala3_jar:$scala_lib_jar"
scalac -nowarn -cp "$CP" -d "$out" \
    "$root"/bindings/scala/runtime/*.scala "$here"/Bench.scala \
    $(find "$gen" -name '*.scala')

exec java -cp "$CP" Bench "$gen/matrix3.desc"
