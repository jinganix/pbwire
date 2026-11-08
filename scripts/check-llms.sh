#!/usr/bin/env bash
# llms.txt sync gate: every public pbw_* function in include/pbwire.h must be
# named in llms.txt, so the AI-retrieval summary cannot drift from the API.
# Usage: scripts/check-llms.sh [--fix-list]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

missing=0
for sym in $(grep -o 'pbw_[a-z_0-9]*(' include/pbwire.h | tr -d '(' | sort -u); do
    # Whole token, not a substring: "pbw_value_get_int" must be documented on
    # its own, not merely as a prefix of "pbw_value_get_int32".
    if ! grep -qE "(^|[^a-zA-Z0-9_])$sym([^a-zA-Z0-9_]|\$)" llms.txt; then
        echo "check-llms: $sym missing from llms.txt"
        missing=1
    fi
done

if [ "$missing" -ne 0 ]; then
    echo "check-llms: add the symbols above to llms.txt"
    exit 1
fi
echo "check-llms: OK"
