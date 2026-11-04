#!/bin/sh
# Builds the pbwire Java binding: plain javac into bindings/java/out.
# No Maven, no Gradle, no JNI, no C glue, no downloads, no native library --
# the binding is pure Java and loads descriptor sets at runtime.
set -eu

here=$(cd "$(dirname "$0")" && pwd)

# Fixtures: the descriptor sets are checked in, so a plain build needs no protoc.
desc=$here/tests/data/sample.desc
if [ ! -f "$desc" ]; then
    protoc --descriptor_set_out="$desc" --include_imports \
        -I "$here/tests/data" "$here/tests/data/sample.proto"
fi

rm -rf "$here/out"
mkdir -p "$here/out"
# shellcheck disable=SC2046 # the source paths hold no whitespace
javac -d "$here/out" $(find "$here/runtime" "$here/examples" "$here/tests" "$here/conformance" -name '*.java')

# Self-check: TestPbwire is the binding gate, TestTyped covers the typed models
# (needs the pinned matrix3 descriptor + bytes, both checked in).
java -cp "$here/out" TestPbwire "$desc"
java -cp "$here/out" TestTyped "$here/../c/tests/data/matrix3.desc" \
    "$here/../c/tests/data/matrix3.bin"

cat <<EOF
build ok: $here/out  (JDK 17+)

example:
  java -cp $here/out Roundtrip ../../examples/addressbook.desc ../../examples/addressbook.bin
EOF
