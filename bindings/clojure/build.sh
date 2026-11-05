#!/bin/sh
# Builds the Clojure binding and runs both self-checks: plain javac of the
# Java runtime it reuses (no Maven, no Gradle), then the clojure.test suite
# and the runnable example on a downloaded Clojure jar.
#
# Needs JDK 17+. Fetches the Clojure jars from Maven Central once (override
# with CLOJURE_JAR / SPEC_ALPHA_JAR / CORE_SPECS_ALPHA_JAR).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
clj_version=${CLOJURE_VERSION:-1.12.0}
spec_version=${SPEC_ALPHA_VERSION:-0.5.238}
core_specs_version=${CORE_SPECS_ALPHA_VERSION:-0.4.74}

depdir=$here/benchmarks/deps
clj_jar=${CLOJURE_JAR:-$depdir/clojure-$clj_version.jar}
spec_jar=${SPEC_ALPHA_JAR:-$depdir/spec.alpha-$spec_version.jar}
core_specs_jar=${CORE_SPECS_ALPHA_JAR:-$depdir/core.specs.alpha-$core_specs_version.jar}

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

cd "$here"

rm -rf "$here/out/java-classes"
mkdir -p "$here/out/java-classes"
javac -nowarn -d "$here/out/java-classes" "$root"/bindings/java/runtime/*.java

base="$clj_jar:$spec_jar:$core_specs_jar:$here/out/java-classes:$here/runtime/src"
java -cp "$base:$here/tests:$here/tests/gen" clojure.main -m pbwire.test-pbwire
java -cp "$base:$here/tests:$here/tests/gen" clojure.main -m generated-test \
    "$root/bindings/c/tests/data/matrix3.bin"
java -cp "$base:$here/examples:$here/examples/gen" clojure.main -m roundtrip \
    "$root/examples/addressbook.bin"

cat <<EOF
build ok: $here/out/java-classes (JDK 17+)

self-check: cd $here && java -cp $base:$here/tests:$here/tests/gen clojure.main -m pbwire.test-pbwire
generated:  cd $here && java -cp $base:$here/tests:$here/tests/gen clojure.main -m generated-test ../../c/tests/data/matrix3.bin
example:    cd $here && java -cp $base:$here/examples:$here/examples/gen clojure.main -m roundtrip ../../examples/addressbook.bin
bench:      ./benchmarks/bench.sh
conformance: cd $here/conformance && java -cp $base:$here/conformance clojure.main -m testee ../../build/conformance.desc
EOF
