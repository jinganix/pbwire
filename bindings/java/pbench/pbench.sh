#!/bin/sh
# pbench for the Java binding: one directory per candidate (pure/ = the shipped
# pure-Java codec, jni/ = the C core through JNI), same payloads, same process,
# interleaved (see README.md).
#
#   ./pbench.sh              # all four tiers
#   TIER=xlarge ./pbench.sh  # one tier, for iteration
#
# Needs JDK 17+ (for jni.h) and a C compiler; links build/libpbwire.a, which it
# builds through the top-level Makefile if missing. Never part of `make test`,
# `make java-test` or CI: it is a comparison harness, not a gate.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
desc=$root/bindings/c/tests/data/matrix3.desc
out=$here/out

if [ ! -f "$desc" ]; then
    echo "missing $desc" >&2
    exit 1
fi

home=${JAVA_HOME:-$(java -XshowSettings:properties -version 2>&1 \
    | awk '/java.home =/ {print $3}')}
if [ -z "$home" ] || [ ! -f "$home/include/jni.h" ]; then
    echo "JAVA_HOME does not point at a JDK with jni.h: '$home'" >&2
    exit 1
fi

case $(uname -s) in
    Darwin) plat=darwin libflag=-dynamiclib ext=dylib ;;
    *) plat=linux libflag=-shared ext=so ;;
esac

make -s -C "$root" build/libpbwire.a

mkdir -p "$out"
rm -rf "${out:?}/classes" "$out/libpbwire_jni.$ext"
${CC:-cc} -O2 -fPIC "$libflag" -o "$out/libpbwire_jni.$ext" \
    -I"$home/include" -I"$home/include/$plat" -I"$root/include" \
    "$here/jni/pbwire_jni.c" "$root/build/libpbwire.a"

# The binding's classes are in the default package (plain javac, no module
# path), so these directories are organization only: one per candidate, like
# bindings/go/pbench/.
javac -d "$out/classes" -nowarn "$root"/bindings/java/runtime/*.java \
    "$here/Compare.java" "$here/pure/PureCandidate.java" "$here/jni/JniCandidate.java"

# Fixed heap + SerialGC keeps allocation/GC noise out of the comparison.
# --enable-native-access keeps JDK 24+ from warning on every loadLibrary and
# from hard-blocking JNI once restricted methods are enforced.
JAVA_FLAGS=${JAVA_FLAGS:--Xms1g -Xmx1g -XX:+UseSerialGC --enable-native-access=ALL-UNNAMED}
exec java $JAVA_FLAGS -cp "$out/classes" -Djava.library.path="$out" \
    Compare "$desc"
