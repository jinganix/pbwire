#!/bin/sh
# Builds the Clojure binding benchmark: plain javac of the Java runtime it
# reuses, protoc --java_out oracle for the protobuf-java baseline, then the
# four standard tiers (BENCHMARKS.md) as MB/s over wire bytes.
#
# Needs JDK 17+ and protoc. Fetches the Clojure and protobuf-java jars from
# Maven Central once (override with CLOJURE_JAR / PROTOBUF_JAVA_JAR).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
clj_version=${CLOJURE_VERSION:-1.12.0}
spec_version=${SPEC_ALPHA_VERSION:-0.5.238}
core_specs_version=${CORE_SPECS_ALPHA_VERSION:-0.4.74}
protobuf_version=${PROTOBUF_JAVA_VERSION:-4.36.2}

depdir=$here/deps
clj_jar=${CLOJURE_JAR:-$depdir/clojure-$clj_version.jar}
spec_jar=${SPEC_ALPHA_JAR:-$depdir/spec.alpha-$spec_version.jar}
core_specs_jar=${CORE_SPECS_ALPHA_JAR:-$depdir/core.specs.alpha-$core_specs_version.jar}
protobuf_jar=${PROTOBUF_JAVA_JAR:-$depdir/protobuf-java-$protobuf_version.jar}

fetch() {
    if [ ! -f "$2" ]; then
        mkdir -p "$depdir"
        echo "fetching $(basename "$2")"
        curl -fsSL -o "$2" "$1"
    fi
}

fetch "https://repo1.maven.org/maven2/org/clojure/clojure/$clj_version/clojure-$clj_version.jar" "$clj_jar"
fetch "https://repo1.maven.org/maven2/org/clojure/spec.alpha/$spec_version/spec.alpha-$spec_version.jar" "$spec_jar"
fetch "https://repo1.maven.org/maven2/org/clojure/core.specs.alpha/$core_specs_version/core.specs.alpha-$core_specs_version.jar" "$core_specs_jar"
fetch "https://repo1.maven.org/maven2/com/google/protobuf/protobuf-java/$protobuf_version/protobuf-java-$protobuf_version.jar" "$protobuf_jar"

if ! command -v protoc >/dev/null 2>&1; then
    echo "protoc is required for the protobuf-java baseline" >&2
    exit 1
fi

rm -rf "$here/out/java-classes" "$here/out/oracle-src" "$here/out/oracle-classes"
mkdir -p "$here/out/java-classes" "$here/out/oracle-src" "$here/out/oracle-classes"
javac -nowarn -d "$here/out/java-classes" "$root"/bindings/java/runtime/*.java
protoc -I"$root/bindings/c/tests/data" --java_out="$here/out/oracle-src" \
    "$root/bindings/c/tests/data/matrix3.proto"
javac -nowarn -cp "$protobuf_jar" -d "$here/out/oracle-classes" \
    $(find "$here/out/oracle-src" -name '*.java')

cp="$clj_jar:$spec_jar:$core_specs_jar:$here/out/java-classes:$here/out/oracle-classes:$protobuf_jar:$here/../runtime/src:$here"
java -cp "$cp" clojure.main -m pbwire.bench "$root/bindings/c/tests/data/matrix3.desc"
