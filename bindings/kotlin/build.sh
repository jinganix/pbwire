#!/bin/sh
# Builds the Kotlin binding: the Java binding's FFM runtime (javac, into
# bindings/java/out) and the Kotlin DSL + self-check + example (kotlinc into
# bindings/kotlin/out/pbwire.jar, with kotlin-stdlib bundled so the run command
# needs no kotlinc directory).
#
# Needs JDK 22+ and kotlinc. No Gradle, no Maven, no downloads.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
java_out=$root/bindings/java/out

make -C "$root" shared >/dev/null

mkdir -p "$java_out" "$here/out"
javac -d "$java_out" "$root"/bindings/java/runtime/*.java

rm -f "$here/out/pbwire.jar"
kotlinc -nowarn -cp "$java_out" -include-runtime -d "$here/out/pbwire.jar" \
    "$here"/runtime/*.kt "$here"/tests/*.kt "$here"/examples/*.kt

case "$(uname -s)" in
Darwin) lib=$root/build/libpbwire.dylib ;;
*) lib=$root/build/libpbwire.so ;;
esac

echo "built $here/out/pbwire.jar"
echo "self-check: java --enable-native-access=ALL-UNNAMED -Dpbwire.library=$lib \\"
echo "                 -cp $here/out/pbwire.jar:$java_out TestPbwireKt $root/bindings/c/tests/data/matrix3.desc"
echo "example:    java --enable-native-access=ALL-UNNAMED -Dpbwire.library=$lib \\"
echo "                 -cp $here/out/pbwire.jar:$java_out RoundtripKt $root/bindings/c/tests/data/matrix3.desc"
