#!/bin/sh
# Builds the pbwire Scala binding and runs its self-checks: the Java
# binding's runtime (javac) plus the Scala DSL, self-check and example
# (scalac). Everything lands in bindings/scala/out/classes.
#
# Needs JDK 17+ and scalac (Scala 2.13+ or 3.x). The fixture descriptor
# sets are checked in, so a build needs no protoc.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

if ! command -v scalac >/dev/null 2>&1; then
    echo "scalac is required (scala 2.13+ or 3.x)" >&2
    exit 1
fi

# scala-library jars for the run classpath: resolved from the scalac
# install (SCALA_CP overrides for exotic layouts).
if [ -z "${SCALA_CP:-}" ]; then
    scalac_bin=$(command -v scalac)
    while [ -L "$scalac_bin" ]; do
        link=$(readlink "$scalac_bin")
        case "$link" in
        /*) scalac_bin=$link ;;
        *) scalac_bin=$(dirname "$scalac_bin")/$link ;;
        esac
    done
    scalac_home=$(cd "$(dirname "$scalac_bin")/.." && pwd)
    # shellcheck disable=SC2046
    SCALA_CP=$(find "$scalac_home/libexec" "$scalac_home/lib" \
        -name 'scala-library*.jar' -o -name 'scala3-library_3-*.jar' \
        2>/dev/null | tr '\n' ':')
fi
CP="$here/out/classes:$SCALA_CP"

# The Java runtime: the only codec in this binding (plain javac, like
# bindings/java/build.sh). No Maven, no Gradle, no JNI, no native library.
rm -rf "$here/out/classes"
mkdir -p "$here/out/classes"
javac -nowarn -d "$here/out/classes" "$root"/bindings/java/runtime/*.java

# The Scala DSL (default package: the Java runtime is not importable from
# a named package), the self-check, the example and the conformance testee.
scalac -nowarn -cp "$here/out/classes" -d "$here/out/classes" \
    "$here"/runtime/*.scala "$here"/tests/*.scala "$here"/tests/gen/*.scala \
    "$here"/examples/*.scala "$here"/examples/gen/*.scala "$here"/conformance/*.scala

# Self-check: TestPbwire is the binding gate (matrix3 oracle bytes plus the
# edge payload); Roundtrip is the addressbook example (generated models).
java -cp "$CP" TestPbwire \
    "$root/bindings/c/tests/data/matrix3.desc" "$root/tests/data/edge.desc"
java -cp "$CP" Roundtrip \
    "$root/examples/addressbook.bin"
java -cp "$CP" GeneratedTestGen \
    "$root/bindings/c/tests/data/matrix3.bin"

cat <<EOF
build ok: $here/out/classes (scalac $(scalac -version 2>&1))

example:
  java -cp $CP Roundtrip $root/examples/addressbook.bin
conformance:
  java -cp $CP Testee <conformance.desc>  # see conformance/ below
EOF
