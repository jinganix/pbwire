#!/usr/bin/env sh
# Validates the first line of a git commit message (Conventional Commits).
# Rules mirror the scope table in CONTRIBUTING.md.

set -e

COMMIT_MSG_FILE="${1:?usage: validate-commit-msg.sh <commit-msg-file>}"
HEADER=$(sed -n '1p' "$COMMIT_MSG_FILE")

# Genuine git-generated merge headers only. A hand-written message like
# "Merge dev into master" is not one: write a conventional message instead
# (e.g. chore(bindings): record master as merged).
case "$HEADER" in
  "Merge branch "*|"Merge remote-tracking branch "*|"Merge remote branch "*|\
  "Merge pull request "*|"Merge commit "*|"Merge tag "*) exit 0
    ;;
  # GitHub's synthetic merge for pull_request CI runs
  # ("Merge <sha> into <sha>"): machine-made, not hand-written.
  "Merge "[0-9a-f]*" into "[0-9a-f]*) exit 0
    ;;
  Merge*)
    echo "commit-msg: hand-written merge message -- use <type>(<scope>): <subject> instead"
    echo "  see CONTRIBUTING.md"
    exit 1
    ;;
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

# A dev-to-master merge must summarize what dev brings in; the subject must
# not be only "merge dev into master" (optionally with a " (#N)" suffix).
SUBJECT=$(printf '%s\n' "$HEADER" | sed -E 's/^[^:]+: //; s/ \(#[0-9]+\)$//')
SUBJECT_LOWER=$(printf '%s\n' "$SUBJECT" | tr 'A-Z' 'a-z')
case "$SUBJECT_LOWER" in
  "merge dev into master"|"merge dev to master"|"merge dev")
    echo "commit-msg: dev-to-master merge must summarize the changes, not just 'merge dev into master'"
    echo "  e.g. feat(bindings): lua binding, typed Go codecs"
    echo "  see CONTRIBUTING.md"
    exit 1
    ;;
esac

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
