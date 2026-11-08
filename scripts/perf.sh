#!/usr/bin/env bash
# Fixed-format perf comparison against libprotobuf, MB/s only.
# Same two tables as BENCHMARKS.md "Core: `pbw_ctx` vs plain vs generated",
# same row/column order, every run:
#   1. payload x op: pbwire ctx / plain / protoc gen / DynamicMessage
#   2. threads: 1-thread and 8-thread aggregate per op
# Usage: scripts/perf.sh
set -euo pipefail

if [ ! -x build/bench ]; then
    make build/bench
fi

./build/bench bench 2>&1 | awk '
/^small:|^full:|^large:|^xlarge:/ {
    section = $1; sub(/:$/, "", section)
    order[++nsec] = section
    secbytes[section] = $2
    inthreads = 0
    next
}
/^threads:/ { inthreads = 1; next }
/^  [^ ]/ {
    line = $0; sub(/^  +/, "", line)
    split(line, f)
    if (f[2] == "encode" || f[2] == "decode") {
        impl = f[1]; op = f[2]; rest0 = 3
    } else {
        impl = f[1] " " f[2]; op = f[3]; rest0 = 4
    }
    if (inthreads) {
        # impl op 1thr-mbps perthread-mbps aggregate-mbps per-op aggregate
        thr1[impl, op] = $(rest0); thragg[impl, op] = $(rest0 + 2)
    } else if (op == "encode" || op == "decode") {
        mbps[section, impl, op] = $NF
    }
    next
}
function cell(v, base) {
    if (base > 0) {
        return sprintf("%.1f (%.2fx)", v, v / base)
    }
    return sprintf("%.1f", v)
}
END {
    printf("| payload | op | pbwire ctx (MB/s, x gen) | pbwire plain (MB/s, x gen) | protoc gen (MB/s) | DynamicMessage (MB/s) |\n")
    printf("|---|---|---|---|---|---|\n")
    for (i = 1; i <= nsec; i++) {
        s = order[i]
        printf("| %s B | encode | %s | %s | %.1f | %.1f |\n", secbytes[s],
            cell(mbps[s, "pbwire c", "encode"], mbps[s, "pb gen", "encode"]),
            cell(mbps[s, "pbwire", "encode"], mbps[s, "pb gen", "encode"]),
            mbps[s, "pb gen", "encode"], mbps[s, "pb dyn", "encode"])
        printf("| %s B | decode | %s | %s | %.1f | %.1f |\n", secbytes[s],
            cell(mbps[s, "pbwire c", "decode"], mbps[s, "pb gen", "decode"]),
            cell(mbps[s, "pbwire", "decode"], mbps[s, "pb gen", "decode"]),
            mbps[s, "pb gen", "decode"], mbps[s, "pb dyn", "decode"])
    }
    printf("\n| op | pbwire 1thr (MB/s, x gen) | pbwire 8thr agg (MB/s, x gen) | gen 1thr (MB/s) | gen 8thr agg (MB/s) |\n")
    printf("|---|---|---|---|---|\n")
    printf("| encode | %s | %s | %.1f | %.1f |\n",
        cell(thr1["pbwire", "encode"], thr1["pb gen", "encode"]),
        cell(thragg["pbwire", "encode"], thragg["pb gen", "encode"]),
        thr1["pb gen", "encode"], thragg["pb gen", "encode"])
    printf("| decode | %s | %s | %.1f | %.1f |\n",
        cell(thr1["pbwire", "decode"], thr1["pb gen", "decode"]),
        cell(thragg["pbwire", "decode"], thragg["pb gen", "decode"]),
        thr1["pb gen", "decode"], thragg["pb gen", "decode"])
}
'
