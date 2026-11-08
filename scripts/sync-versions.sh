#!/usr/bin/env bash
# Single source of truth for every release version: the repo-root VERSION file.
#
#   scripts/sync-versions.sh          rewrite every static version from VERSION
#   scripts/sync-versions.sh --check verify everything matches VERSION (CI gate)
#
# Files that read VERSION at build time (setup.py, *.gemspec, *.podspec) cannot
# drift; the script only asserts they still reference it. Everything else is
# rewritten in place and must be committed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VER="$(tr -d ' \t\n' < VERSION)"
if [[ ! "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "sync-versions: VERSION '$VER' is not X.Y.Z" >&2
  exit 1
fi

CHECK=0
if [[ "${1:-}" == "--check" ]]; then
  CHECK=1
fi
FAIL=0
fail() { echo "sync-versions: drift: $1" >&2; FAIL=1; }

# set <file> <sed-expr...>: rewrite (or verify in --check mode).
setver() {
  local file=$1; shift
  if [[ $CHECK == 1 ]]; then
    return 0
  fi
  sed -i.bak "$@" "$file" && rm -f "$file.bak"
}

expect() { # expect <file> <grep-pattern> <what>
  if ! grep -qE "$2" "$1"; then
    fail "$3 ($1)"
  fi
}

if [[ $CHECK == 1 ]]; then
  expect CMakeLists.txt "^project\\(pbwire VERSION $VER" "cmake project version"
  expect Makefile "-current_version $VER" "Makefile (darwin -current_version)"
  expect bindings/dart/pubspec.yaml "^version: $VER\$" "dart pubspec"
  expect bindings/dart/benchmarks/pubspec.yaml "^version: $VER\$" "dart bench pubspec"
  expect bindings/dart/benchmarks/pubspec.lock "version: \"$VER\"" "dart bench lock path dep"
  expect bindings/rust/Cargo.toml "^version = \"$VER\"\$" "rust Cargo.toml"
  expect bindings/rust/benchmarks/Cargo.toml "^version = \"$VER\"\$" "rust bench Cargo.toml"
  expect bindings/erlang/src/pbwire.app.src "{vsn, \"$VER\"}" "erlang .app.src"
  expect bindings/csharp/Pbwire/Pbwire.csproj "<Version>$VER</Version>" "csharp csproj"
  expect bindings/node/package.json "\"version\": \"$VER\"" "node package.json"
  expect bindings/java/pom.xml "<version>$VER</version>" "java pom"
  expect bindings/kotlin/pom.xml "<version>$VER</version>" "kotlin pom"
  expect bindings/elixir/mix.exs "version: \"$VER\"" "elixir mix.exs"
  expect bindings/fsharp/runtime/Pbwire.FSharp.fsproj "<Version>$VER</Version>" "fsharp fsproj"
  expect bindings/scala/build.sbt "ThisBuild / version := \"$VER\"" "scala build.sbt"
  expect bindings/julia/runtime/Project.toml "^version = \"$VER\"$" "julia Project.toml"
  expect bindings/clojure/deps.edn ":artifact \"target/pbwire-clojure-$VER.jar\"" "clojure deps.edn"
  expect bindings/clojure/pom.xml "^  <version>$VER</version>" "clojure pom template"
  expect bindings/r/runtime/DESCRIPTION "^Version: $VER\$" "r DESCRIPTION"
  # Plugin versions must never track VERSION (a blanket rewrite once
  # shipped maven-compiler-plugin:0.1.1 to every user of the file).
  if grep -A1 "maven-compiler-plugin" bindings/java/pom.xml | grep -q "<version>$VER</version>"; then
    fail "java pom: plugin version must not track VERSION"
  fi
  expect README.md "releases/tag/v$VER" "README release line"
  expect llms.txt "version \`$VER\`" "llms.txt version"
  rock="bindings/lua/pbwire-$VER-1.rockspec"
  [ -f "$rock" ] || fail "lua rockspec missing ($rock)"
  expect "$rock" "^version = \"$VER-1\"$" "lua rockspec version"
  expect "$rock" "refs/tags/v$VER.tar.gz" "lua rockspec source url"
  for f in bindings/python/setup.py bindings/ruby/pbwire.gemspec bindings/objc/PBWire.podspec bindings/clojure/build.clj; do
    grep -q "VERSION" "$f" || fail "$f: no longer reads the VERSION file"
  done
  # These carry no version field by design (registry versions come from git tags).
  grep -q '"version"' composer.json && fail "composer.json: must not hardcode a version"
  if [[ $FAIL == 1 ]]; then exit 1; fi
  echo "sync-versions: all versions are $VER"
  exit 0
fi

setver CMakeLists.txt -e "s/^project[(]pbwire VERSION [0-9.]*/project(pbwire VERSION $VER/"
setver Makefile -e "s/-current_version [0-9.]*/-current_version $VER/"
setver bindings/dart/pubspec.yaml -e "s/^version: .*/version: $VER/"
setver bindings/dart/benchmarks/pubspec.yaml -e "s/^version: .*/version: $VER/"
setver bindings/rust/Cargo.toml -e "s/^version = \".*\"/version = \"$VER\"/"
setver bindings/rust/benchmarks/Cargo.toml -e "s/^version = \".*\"/version = \"$VER\"/"
setver bindings/erlang/src/pbwire.app.src -e "s/{vsn, \"[^\"]*\"}/{vsn, \"$VER\"}/"
setver bindings/csharp/Pbwire/Pbwire.csproj -e "s|<Version>[^<]*</Version>|<Version>$VER</Version>|"
setver bindings/node/package.json -e "s/\"version\": \"[^\"]*\"/\"version\": \"$VER\"/"
# The project version is the only two-space <version> in each pom. Plugin
# versions live deeper and must never be touched (a blanket rewrite once
# shipped maven-compiler-plugin:0.1.1 to every user of the file).
setver bindings/java/pom.xml -e "s|^  <version>[0-9.]*</version>|  <version>$VER</version>|"
setver bindings/kotlin/pom.xml -e "s|^  <version>[0-9.]*</version>|  <version>$VER</version>|"
setver bindings/elixir/mix.exs -e "s/version: \"[0-9.]*/version: \"$VER/"
setver bindings/fsharp/runtime/Pbwire.FSharp.fsproj -e "s|<Version>[^<]*</Version>|<Version>$VER</Version>|"
setver bindings/scala/build.sbt -e "s|ThisBuild / version := \"[0-9.]*\"|ThisBuild / version := \"$VER\"|"
setver bindings/julia/runtime/Project.toml -e "s/^version = \".*\"/version = \"$VER\"/"
setver bindings/clojure/deps.edn -e "s|target/pbwire-clojure-[0-9.]*.jar|target/pbwire-clojure-$VER.jar|"
setver bindings/clojure/pom.xml -e "s|^  <version>[0-9.]*</version>|  <version>$VER</version>|"
setver bindings/r/runtime/DESCRIPTION -e "s/^Version: .*/Version: $VER/"
# Kotlin pins its pbwire-java dependency to the same release line.
setver bindings/kotlin/pom.xml -e "/<artifactId>pbwire-java<\/artifactId>/,/<\/dependency>/s|<version>[0-9.]*</version>|<version>$VER</version>|"
setver README.md -e "s|releases/tag/v[0-9.]*|releases/tag/v$VER|g" \
  -e "s|\*\*Release \[\`v[0-9.]*\`\]|**Release [\`v$VER\`]|"
setver llms.txt -e "s|version \`[0-9.]*\`|version \`$VER\`|"
# The rockspec filename carries the version, so a bump renames it.
old_rock=$(echo bindings/lua/pbwire-*-*.rockspec)
new_rock="bindings/lua/pbwire-$VER-1.rockspec"
if [[ "$old_rock" != "$new_rock" ]]; then
  mv "$old_rock" "$new_rock"
fi
setver "$new_rock" -e "s/^version = \".*\"/version = \"$VER-1\"/" \
  -e "s|refs/tags/v[0-9.]*.tar.gz|refs/tags/v$VER.tar.gz|" \
  -e "s|^  dir = \"pbwire-[0-9.]*\"|  dir = \"pbwire-$VER\"|"

echo "sync-versions: synced everything to $VER"
