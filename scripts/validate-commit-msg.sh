#!/usr/bin/env sh
# Validates the first line of a git commit message (Conventional Commits).
# Rules mirror the scope table in CONTRIBUTING.md.

set -e

COMMIT_MSG_FILE="${1:?usage: validate-commit-msg.sh <commit-msg-file>}"
HEADER=$(sed -n '1p' "$COMMIT_MSG_FILE")

# Merge commits
case "$HEADER" in
  Merge*) exit 0
esac

TYPES='feat|fix|docs|style|refactor|perf|test|build|ci|chore|revert'
SCOPES='core|protoc|test|docs|build|ci|bindings'
PATTERN="^(${TYPES})\\((${SCOPES})\\): .+"

if ! printf '%s\n' "$HEADER" | grep -qE "$PATTERN"; then
  echo "commit-msg: invalid header -- expected: <type>(<scope>): <subject>"
  echo "  types:  feat fix docs style refactor perf test build ci chore revert"
  echo "  scopes: core protoc test docs build ci bindings"
  echo "  see CONTRIBUTING.md"
  exit 1
fi

if [ "${#HEADER}" -gt 100 ]; then
  echo "commit-msg: header exceeds 100 characters (${#HEADER})"
  exit 1
fi

# English-only: the commit message must be ASCII (no CJK / accented / emoji).
# LC_ALL=C makes [:print:] / [:space:] ASCII-only, so any non-ASCII byte matches.
if LC_ALL=C grep -q '[^[:print:][:space:]]' "$COMMIT_MSG_FILE"; then
  echo "commit-msg: non-ASCII characters found -- use English only (ASCII)"
  echo "  see CONTRIBUTING.md"
  exit 1
fi

exit 0
