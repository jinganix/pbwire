#!/bin/sh
# Builds the pbwire Java binding: plain javac into bindings/java/out.
# No Maven, no Gradle, no JNI, no C glue, no native library --
# the binding is pure Java and loads descriptor sets at runtime.
#
# The typed compat test also compiles the protobuf-java oracle (the T4/T5
# baseline): protoc regenerates it from the same .proto files and the jar
# below is the pinned version (see bindings/typed/testdata/ORACLES.md).
# Override with PROTOBUF_JAVA_JAR / PROTOBUF_JAVA_VERSION.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
version=${PROTOBUF_JAVA_VERSION:-4.36.2}
jar=${PROTOBUF_JAVA_JAR:-$here/benchmarks/deps/protobuf-java-$version.jar}

if [ ! -f "$jar" ]; then
    mkdir -p "$(dirname "$jar")"
    echo "fetching protobuf-java $version (typed-test oracle only)"
    curl -fsSL -o "$jar" \
        "https://repo1.maven.org/maven2/com/google/protobuf/protobuf-java/$version/protobuf-java-$version.jar"
fi
if ! command -v protoc >/dev/null 2>&1; then
    echo "protoc is required for the typed-test oracle" >&2
    exit 1
fi

# Fixtures: the descriptor sets are checked in, so a plain build needs no protoc.
desc=$here/tests/data/sample.desc
if [ ! -f "$desc" ]; then
    protoc --descriptor_set_out="$desc" --include_imports \
        -I "$here/tests/data" "$here/tests/data/sample.proto"
fi

# Oracle sources from the same .proto files the facade is generated from.
oracle=$here/out/oracle-src
mkdir -p "$oracle"
rm -rf "${oracle:?}/structtest" "${oracle:?}/spectest"
protoc -I"$root/bindings/c/tests/data" --java_out="$oracle" \
    "$root/bindings/c/tests/data/matrix3.proto"
protoc -I"$root/bindings/typed/testdata" --java_out="$oracle" \
    "$root/bindings/typed/testdata/oneof1.proto"

rm -rf "$here/out/classes"
mkdir -p "$here/out/classes"
# shellcheck disable=SC2046 # the source paths hold no whitespace
javac -nowarn -cp "$jar" -d "$here/out/classes" \
    $(find "$here/runtime" "$here/examples" "$here/tests" "$here/conformance" "$oracle" -name '*.java')

# Self-check: TestPbwire is the binding gate, TestTyped covers the typed models
# (T1-T6 against the pinned reference bytes and the protobuf-java oracle).
java -cp "$here/out/classes" TestPbwire "$desc"
java -cp "$here/out/classes:$jar" TestTyped \
    "$root/bindings/c/tests/data/matrix3.bin" "$root/bindings/typed/testdata/oneof1.bin"
java -cp "$here/out/classes" Roundtrip "$root/examples/addressbook.bin"

cat <<EOF
build ok: $here/out/classes  (JDK 17+)

example:
  java -cp $here/out/classes Roundtrip ../../examples/addressbook.bin
EOF
