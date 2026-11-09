#!/bin/sh
# Run a benchmark, keep its log, and record one structured JSON file.
# The bench output on stdout is unchanged; the record never masks the
# bench exit status.
#
#   scripts/bench-run.sh --binding NAME -- CMD...
#
# Writes results/bench-NAME-<utc-stamp>.{log,json}. The record step needs
# python3; without it the log is still kept and the bench still passes.
set -eu

binding=""
while [ $# -gt 0 ]; do
    case "$1" in
        --binding) binding="$2"; shift 2 ;;
        --) shift; break ;;
        *) echo "usage: bench-run.sh --binding NAME -- CMD..." >&2; exit 2 ;;
    esac
done
if [ -z "$binding" ] || [ $# -eq 0 ]; then
    echo "usage: bench-run.sh --binding NAME -- CMD..." >&2
    exit 2
fi

mkdir -p results
stamp=$(date -u +%Y%m%dT%H%M%SZ)
log="results/bench-$binding-$stamp.log"
out="results/bench-$binding-$stamp.json"

start=$(date +%s)
if "$@" >"$log" 2>&1; then
    st=0
else
    st=$?
fi
end=$(date +%s)
cat "$log"

if command -v python3 >/dev/null 2>&1; then
    if python3 scripts/bench-record.py --binding "$binding" \
            --log "$log" --out "$out" --status "$st" \
            --elapsed $((end - start)); then
        echo "bench record: $out"
    else
        echo "bench-run: no benchmark blocks parsed, log kept at $log" >&2
    fi
else
    echo "bench-run: python3 not found, log kept at $log" >&2
fi
exit $st
