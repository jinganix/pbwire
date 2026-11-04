#!/bin/sh
# Run the standard benchmark and refresh the README result sections.
set -e

cd "$(dirname "$0")"
TOOL_SH="$(cd .. && pwd)/scripts/with_tool_versions.sh"

MARKER_START='<!-- BENCHMARK_RESULTS_START -->'
MARKER_END='<!-- BENCHMARK_RESULTS_END -->'

if [ -n "$BENCH_TARGET_TIME" ]; then
  echo "Using BENCH_TARGET_TIME=${BENCH_TARGET_TIME} (shorter run)"
fi

echo "[update-readme] building..."
make build >/dev/null
make benchmarks-nif-code >/dev/null 2>&1 || echo "[update-readme] gpb NIF baseline not built (needs protoc + libprotobuf)"

HW_INFO=$(make show-hw-sw-info 2>&1)
RUN_DATE=$(date -u '+%Y-%m-%d %H:%M:%S UTC')
MACHINE_MODEL=$(echo "$HW_INFO" | awk '
  /^CPU info$/ { in_cpu=1; next }
  in_cpu && /^$/ { exit }
  in_cpu && /^model name[[:space:]]*:/ {
    sub(/^model name[[:space:]]*:[[:space:]]*/, "", $0)
    print
    exit
  }
  in_cpu && $0 !~ /^(physical cpus|logical cpus|cache size|cores\/threads|bogomips)[[:space:]]*:/ {
    print
    exit
  }
')
[ -n "$MACHINE_MODEL" ] || MACHINE_MODEL="unknown"

LOG=$(mktemp)
trap 'rm -f "$LOG"' EXIT INT TERM

echo "[update-readme] running benchmarks..."
"$TOOL_SH" ./proto-bench 2>&1 | tee "$LOG"
OUT=$(cat "$LOG")

ERLANG_VERSION=$("$TOOL_SH" --erlang-version)

TABLE=$(echo "$OUT" | awk '
  /^(small|medium|large|xlarge): [0-9]+ bytes, / {
    case_name = $1
    sub(/:$/, "", case_name)
    size[case_name] = $2
    next
  }
  /^  (pbwire-erlang|gpb|gpb-nif)[[:space:]]+(encode|decode)[[:space:]]+[0-9.]+/ {
    value[case_name, $2, $1] = $4
  }
  END {
    split("small medium large xlarge", cases, " ")
    split("encode decode", ops, " ")
    split("pbwire-erlang gpb gpb-nif", impls, " ")
    for (i = 1; i <= 4; i++) {
      c = cases[i]
      for (j = 1; j <= 2; j++) {
        o = ops[j]
        e = value[c, o, impls[1]]
        g = value[c, o, impls[2]]
        n = value[c, o, impls[3]]
        r = (e != "" && g != "") ? sprintf("%.2fx", e / g) : "—"
        printf "| %s B | %s | %s (%s) | %s | %s |\n", size[c], o, \
          (e == "" ? "—" : e), r, (g == "" ? "—" : g), (n == "" ? "—" : n)
      }
    }
  }
')

TABLE_HEAD='| payload | op | pbwire-erlang (MB/s, x gpb) | gpb (MB/s) | gpb-nif (MB/s) |
|---|---|---|---|---|'

TABLE_FOOTER='Higher is better. Throughput in MB/s on serialized wire size. Case names are the
four standard tiers at the repo root ([BENCHMARKS.md](BENCH_LINK)).'

table_block() {
  link=$1
  footer=$(echo "$TABLE_FOOTER" | sed "s|BENCH_LINK|$link|")
  cat <<EOF
Last run: ${RUN_DATE} (matrix3.proto, the four standard tiers; Erlang ${ERLANG_VERSION} per \`.tool-versions\`; machine ${MACHINE_MODEL})

${TABLE_HEAD}
${TABLE}

${footer}
EOF
}

RESULTS_FILE=$(mktemp)
SUMMARY_FILE=$(mktemp)
cat > "$RESULTS_FILE" <<EOF
$MARKER_START
$(table_block "../../../BENCHMARKS.md")

<details>
<summary>Hardware / OTP (last run)</summary>

\`\`\`
${HW_INFO}
\`\`\`
</details>

<details>
<summary>Full benchmark log (last run)</summary>

\`\`\`
${OUT}
\`\`\`
</details>
$MARKER_END
EOF

cat > "$SUMMARY_FILE" <<EOF
$MARKER_START
$(table_block "../../BENCHMARKS.md")
$MARKER_END
EOF

patch_file() {
  target=$1
  block_file=$2
  if [ ! -f "$target" ]; then
    echo "skip missing $target"
    return 0
  fi
  tmp=$(mktemp)
  if grep -q "$MARKER_START" "$target"; then
    before=$(grep -n "$MARKER_START" "$target" | head -1 | cut -d: -f1)
    after=$(grep -n "$MARKER_END" "$target" | head -1 | cut -d: -f1)
    head -n $((before - 1)) "$target" > "$tmp"
    cat "$block_file" >> "$tmp"
    tail -n +$((after + 1)) "$target" >> "$tmp"
  else
    cp "$target" "$tmp"
    printf '\n' >> "$tmp"
    cat "$block_file" >> "$tmp"
  fi
  mv "$tmp" "$target"
  echo "updated $target"
}

patch_file README.md "$RESULTS_FILE"
patch_file ../README.md "$SUMMARY_FILE"

rm -f "$RESULTS_FILE" "$SUMMARY_FILE"

echo "Benchmark summary:"
echo "$TABLE"
