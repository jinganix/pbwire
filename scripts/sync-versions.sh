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
  expect bindings/rust/Cargo.toml "^version = \"$VER\"\$" "rust Cargo.toml"
  expect bindings/rust/benchmarks/Cargo.toml "^version = \"$VER\"\$" "rust bench Cargo.toml"
  expect bindings/erlang/src/pbwire.app.src "{vsn, \"$VER\"}" "erlang .app.src"
  expect bindings/csharp/Pbwire/Pbwire.csproj "<Version>$VER</Version>" "csharp csproj"
  expect bindings/node/package.json "\"version\": \"$VER\"" "node package.json"
  expect bindings/java/pom.xml "<version>$VER</version>" "java pom"
  expect bindings/kotlin/pom.xml "<version>$VER</version>" "kotlin pom"
  expect README.md "releases/tag/v$VER" "README release line"
  for f in bindings/python/setup.py bindings/ruby/pbwire.gemspec bindings/objc/PBWire.podspec; do
    grep -q "VERSION" "$f" || fail "$f: no longer reads the VERSION file"
  done
  # These carry no version field by design (registry versions come from git tags).
  grep -q '"version"' bindings/php/composer.json && fail "composer.json: must not hardcode a version"
  if [[ $FAIL == 1 ]]; then exit 1; fi
  echo "sync-versions: all versions are $VER"
  exit 0
fi

setver CMakeLists.txt -e "s/^project(pbwire VERSION )[0-9.]*/project(pbwire VERSION $VER/"
setver Makefile -e "s/-current_version [0-9.]*/-current_version $VER/"
setver bindings/dart/pubspec.yaml -e "s/^version: .*/version: $VER/"
setver bindings/dart/benchmarks/pubspec.yaml -e "s/^version: .*/version: $VER/"
setver bindings/rust/Cargo.toml -e "s/^version = \".*\"/version = \"$VER\"/"
setver bindings/rust/benchmarks/Cargo.toml -e "s/^version = \".*\"/version = \"$VER\"/"
setver bindings/erlang/src/pbwire.app.src -e "s/{vsn, \"[^\"]*\"}/{vsn, \"$VER\"}/"
setver bindings/csharp/Pbwire/Pbwire.csproj -e "s|<Version>[^<]*</Version>|<Version>$VER</Version>|"
setver bindings/node/package.json -e "s/\"version\": \"[^\"]*\"/\"version\": \"$VER\"/"
setver bindings/java/pom.xml -e "s|<version>[0-9.]*</version>|<version>$VER</version>|g"
setver bindings/kotlin/pom.xml -e "s|<version>[0-9.]*</version>|<version>$VER</version>|g"
setver README.md -e "s|releases/tag/v[0-9.]*|releases/tag/v$VER|g" \
  -e "s|\*\*Release \[\`v[0-9.]*\`\]|**Release [\`v$VER\`]|"

echo "sync-versions: synced everything to $VER"
