#!/bin/sh
# Builds the Kotlin binding and runs both self-checks: the Java binding's
# runtime (javac) plus the facade/oracle Java classes, and the Kotlin DSL +
# self-check + example (kotlinc). Everything lands in one fat
# bindings/kotlin/out/pbwire.jar (kotlin-stdlib, facade, oracle and the
# protobuf-java runtime inside), so the run commands need no extra classpath.
#
# Needs JDK 17+, kotlinc and protoc. The protobuf-java jar below is the pinned
# T4/T5 oracle (see bindings/typed/testdata/ORACLES.md); override with
# PROTOBUF_JAVA_JAR / PROTOBUF_JAVA_VERSION.
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

make -C "$root" shared >/dev/null

# Oracle sources from the same .proto files the facade is generated from.
oracle=$here/out/oracle-src
mkdir -p "$oracle"
rm -rf "${oracle:?}/structtest" "${oracle:?}/spectest"
protoc -I"$root/bindings/c/tests/data" --java_out="$oracle" \
    "$root/bindings/c/tests/data/matrix3.proto"
protoc -I"$root/bindings/typed/testdata" --java_out="$oracle" \
    "$root/bindings/typed/testdata/oneof1.proto"

rm -rf "$here/out/java-classes" "$here/out/oracle-classes"
mkdir -p "$here/out/java-classes" "$here/out/oracle-classes"
# The facade is the Java binding's generated code: Kotlin reuses those classes.
javac -nowarn -d "$here/out/java-classes" \
    "$root"/bindings/java/runtime/*.java \
    "$root"/bindings/java/tests/gen/*.java "$root"/bindings/java/examples/gen/*.java
javac -nowarn -cp "$jar" -d "$here/out/oracle-classes" \
    $(find "$oracle" -name '*.java')

# One fat jar: kotlin classes + stdlib, facade, oracle, protobuf-java.
rm -f "$here/out/pbwire.jar"
kotlinc -nowarn -cp "$here/out/java-classes:$here/out/oracle-classes:$jar" \
    -include-runtime -d "$here/out/pbwire.jar" \
    "$here"/runtime/*.kt "$here"/tests/*.kt "$here"/tests/gen/*.kt \
    "$here"/examples/*.kt "$here"/examples/gen/*.kt
stage=$here/out/fat-stage
rm -rf "$stage"
mkdir -p "$stage"
cd "$stage"
jar xf "$here/out/pbwire.jar"
cp -r "$here/out/java-classes/"* .
cp -r "$here/out/oracle-classes/"* .
jar xf "$jar"
jar cf "$here/out/pbwire.jar" .
cd "$here"

case "$(uname -s)" in
Darwin) lib=$root/build/libpbwire.dylib ;;
*) lib=$root/build/libpbwire.so ;;
esac

java --enable-native-access=ALL-UNNAMED -Dpbwire.library="$lib" \
    -cp "$here/out/pbwire.jar:$here/out/java-classes" TestPbwireKt \
    "$root/bindings/c/tests/data/matrix3.desc"
java --enable-native-access=ALL-UNNAMED -Dpbwire.library="$lib" \
    -cp "$here/out/pbwire.jar:$here/out/java-classes" TypedTestKt \
    "$root/bindings/c/tests/data/matrix3.bin" "$root/bindings/typed/testdata/oneof1.bin"
java --enable-native-access=ALL-UNNAMED -Dpbwire.library="$lib" \
    -cp "$here/out/pbwire.jar:$here/out/java-classes" RoundtripKt \
    "$root/examples/addressbook.bin"

echo "built $here/out/pbwire.jar"
echo "self-check: java --enable-native-access=ALL-UNNAMED -Dpbwire.library=$lib \\"
echo "                 -cp $here/out/pbwire.jar:$here/out/java-classes TestPbwireKt $root/bindings/c/tests/data/matrix3.desc"
echo "example:    java --enable-native-access=ALL-UNNAMED -Dpbwire.library=$lib \\"
echo "                 -cp $here/out/pbwire.jar:$here/out/java-classes RoundtripKt $root/examples/addressbook.bin"
