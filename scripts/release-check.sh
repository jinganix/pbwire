#!/usr/bin/env bash
# Release dry-run: everything `git tag vX.Y.Z` assumes, without tagging.
#
#   ./scripts/release-check.sh            full dry-run
#   ./scripts/release-check.sh --offline  skip network checks (tarball URLs)
#
# Checks: VERSION is X.Y.Z, sync-versions --check passes, HEAD is on a
# non-master branch state that is mergeable (warns on dirty tree), the tag
# (when present) matches VERSION, CHANGELOG has an entry for VERSION, the
# shared library builds, and every binding artifact the release workflow
# uploads still has its build entry point.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

OFFLINE=0
[[ "${1:-}" == "--offline" ]] && OFFLINE=1

VER="$(tr -d ' \t\n' < VERSION)"
[[ "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "release-check: bad VERSION '$VER'"; exit 1; }
echo "release-check: VERSION=$VER"

echo "== versions =="
./scripts/sync-versions.sh --check

echo "== changelog =="
grep -q "## \[$VER\]" CHANGELOG.md || { echo "release-check: CHANGELOG.md has no [$VER] entry"; exit 1; }

echo "== tag =="
if git rev-parse "v$VER" >/dev/null 2>&1; then
  echo "release-check: tag v$VER exists"
else
  echo "release-check: tag v$VER not yet created (expected before publish)"
fi

echo "== tree =="
if ! git diff --quiet || ! git diff --cached --quiet; then
  echo "release-check: WARN: dirty working tree"
fi
if [[ "$(git branch --show-current)" == "master" ]]; then
  echo "release-check: WARN: on master; release from a feature branch per AGENTS.md"
fi

echo "== build =="
make -s shared

echo "== binding entry points =="
# Each line: <label> <command...>. Only presence/executability is checked;
# the release workflow does the real builds.
check() { command -v "$1" >/dev/null 2>&1 || { echo "release-check: missing tool: $1 ($2)"; exit 1; }; }
[ -f bindings/python/setup.py ] || { echo "release-check: bindings/python/setup.py gone"; exit 1; }
[ -f bindings/ruby/pbwire.gemspec ] || { echo "release-check: gemspec gone"; exit 1; }
[ -f bindings/node/package.json ] || { echo "release-check: node package.json gone"; exit 1; }
[ -f bindings/rust/Cargo.toml ] || { echo "release-check: rust Cargo.toml gone"; exit 1; }
[ -f bindings/java/pom.xml ] || { echo "release-check: java pom.xml gone"; exit 1; }
[ -f CMakeLists.txt ] || { echo "release-check: CMakeLists.txt gone"; exit 1; }
echo "release-check: entry points present"

if [[ $OFFLINE == 0 ]]; then
  echo "== tarball URL =="
  if command -v curl >/dev/null 2>&1; then
    curl -sSf -o /dev/null -I "https://github.com/jinganix/pbwire/archive/refs/tags/v$VER.tar.gz" \
      && echo "release-check: tarball URL live" \
      || echo "release-check: WARN: tarball URL not live (tag not pushed?)"
  else
    echo "release-check: WARN: curl missing, skipping URL check"
  fi
fi

echo "release-check: OK (dry-run; nothing was tagged or published)"
