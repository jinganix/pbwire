#!/bin/sh
# Builds the pbwire Java binding: the C shared library, then javac into
# bindings/java/out. No Maven, no Gradle, no JNI, no C glue, no downloads.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

# The Foreign Function & Memory API dlopen()s a shared object, so the binding
# needs `make shared`, not the static build/libpbwire.a.
make -C "$root" shared

case "$(uname -s)" in
Darwin) lib=$root/build/libpbwire.dylib ;;
*) lib=$root/build/libpbwire.so ;;
esac

# Fixture: the descriptor set is checked in, so a plain build needs no protoc.
desc=$here/tests/data/sample.desc
if [ ! -f "$desc" ]; then
    protoc --descriptor_set_out="$desc" --include_imports \
        -I "$here/tests/data" "$here/tests/data/sample.proto"
fi

rm -rf "$here/out"
mkdir -p "$here/out"
# shellcheck disable=SC2046 # the source paths hold no whitespace
javac -d "$here/out" $(find "$here/runtime" "$here/examples" "$here/tests" -name '*.java')

cat <<EOF
build ok: $here/out  (JDK 22+, links $lib)

self-check:
  java --enable-native-access=ALL-UNNAMED -Dpbwire.library=$lib -cp $here/out TestPbwire $desc

example:
  java --enable-native-access=ALL-UNNAMED -Dpbwire.library=$lib -cp $here/out Roundtrip $desc
EOF
