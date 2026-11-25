#!/usr/bin/env bash
# Anchor freeze + drift check for the benchmark docs (plan Task D1).
#
# What it checks:
#   (a) every tier-table section (BENCHMARKS.md `###` sections and each
#       binding README's `## Benchmarks` section, exactly the set
#       `bench-charts.py:collect_sections()` renders) slug-matches
#       (per `slug_of()`) an existing `docs/charts/bench-<slug>.svg`,
#       and the owning file embeds that chart; every BENCHMARKS.md stub
#       section ("Full table and method:" window, same rule the chart
#       script inserts under) embeds its chart too;
#   (b) `ORDER` in `scripts/bench-compare.py` matches the at-a-glance row
#       order in BENCHMARKS.md (deduped: the two core rows collapse to one);
#   (c) every ratio cell in the at-a-glance table, the per-binding `xlarge:`
#       one-liners, the "Bindings vs their baselines" xlarge column, the
#       binding README tier tables, and the top-level README picker table
#       matches its cited `results/bench-<lang>-20261124*.json` formal run
#       within rounding (markers: displayed ratio >= 1.0 <=> ✅).
#
# Ratio rules (c): a tier-table cell needs doc-internal math
# (|ours/base - ratio| <= 0.006) plus both MB/s values present in the cited
# run (same payload bytes + op, any pass block: the PHP/Swift harnesses print
# repeated passes per baseline); a summary cell without a printed baseline
# needs a same-case+op (ours, base) pair in the run whose quotient rounds to
# the printed ratio. The cited run is the `results/bench-<stamp>.json` the
# binding README names, else the newest `20261124*` record (C and Go name
# none; the newest matches their tables).
#
# Out of scope on purpose: the Linux/docker paired tables (their source is a
# macOS+Linux record *pair*, a different check), the embedded ns table (no
# cited JSON, different domain), prose ratios (D3 dedups those), and
# `pbench/` arm tables (non-formal runs).
#
# Invocation: ./scripts/check-bench-refs.sh
# Run it before and after every BENCHMARKS.md / README / binding-README edit
# (plan Tasks D2-D4). Exit 0 = green (KNOWN-EXCEPTION lines are pre-existing
# drift being tracked, not passes); exit 1 = drift with file:line details.
#
# NOT wired into scripts/check.sh on purpose: results/ is git-ignored, so a
# fresh checkout has no JSON records and part (c) would have nothing to check.
# With no results/*.json present, part (c) reports SKIP and still exits 0;
# parts (a) and (b) always run. Needs only bash + python3 (stdlib); no new dependencies.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

python3 - "$ROOT" <<'PYEOF'
import ast
import glob
import importlib.util
import json
import os
import re
import sys

ROOT = sys.argv[1]
BENCH = os.path.join(ROOT, "BENCHMARKS.md")
TOP_README = os.path.join(ROOT, "README.md")
CHART_DIR = os.path.join(ROOT, "docs", "charts")
RESULTS = os.path.join(ROOT, "results")

spec = importlib.util.spec_from_file_location(
    "bench_charts", os.path.join(ROOT, "scripts", "bench-charts.py"))
bc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bc)

RATIO = re.compile(r"(\d+\.\d+)x\s*(✅|❌)?")
MBPS = re.compile(r"\d+\.\d+")
fails = []
notices = []

# ---------- shared JSON helpers (check c) ----------
JSCACHE = {}


def jload(path):
    if path not in JSCACHE:
        with open(path, encoding="utf-8") as f:
            JSCACHE[path] = json.load(f)
    return JSCACHE[path]


def cited_run(prefix, readme_path=None):
    """The formal run a binding's tables are pasted from."""
    cands = sorted(glob.glob(os.path.join(
        RESULTS, "bench-%s-20261124*.json" % prefix)))
    if not cands:
        return None
    if readme_path and os.path.isfile(readme_path):
        with open(readme_path, encoding="utf-8") as f:
            m = re.search(r"results/bench-" + re.escape(prefix)
                          + r"-([0-9TZ]+)\.json", f.read())
        if m:
            p = os.path.join(RESULTS, "bench-%s-%s.json" % (prefix, m.group(1)))
            if os.path.isfile(p):
                return p
            fails.append("%s: cited run %s is missing" % (readme_path, p))
            return None
    return cands[-1]


def json_groups(prefix, readme_path=None):
    """{(bytes, op): [mbps]} across every pass block of the cited run."""
    jp = cited_run(prefix, readme_path)
    if jp is None:
        return None, None
    groups = {}
    for case in jload(jp)["cases"]:
        for row in case["rows"]:
            groups.setdefault((case["bytes"], row["op"]), []).append(row["mbps"])
    return jp, groups


def runs_of(prefix):
    return RUN.get(prefix, [])


def numbers_in(groups, case_bytes, op, ours, base):
    return any(abs(v - ours) <= 0.06 and any(abs(w - base) <= 0.06 for w in vals)
               for (b, o), vals in groups.items()
               for v in vals if b == case_bytes and o == op)


def pair_in(groups, case_bytes, ours, ratio, op_hint):
    """A same-case+op (ours, base) pair in the run quotients to ratio."""
    for (b, op), vals in groups.items():
        if b != case_bytes:
            continue
        if op_hint == "encode" and "encode" not in op:
            continue
        if op_hint == "decode" and not ("decode" in op or "dict" in op):
            continue
        for i, o in enumerate(vals):
            if abs(o - ours) > 0.06:
                continue
            for j, base in enumerate(vals):
                if i != j and abs(o / base - ratio) <= 0.006:
                    return True
    return False


def check_tier_row(prefix, case_bytes, op, ours, ratio, marker, bases, where):
    """Full tier-table cell: doc math + both numbers traceable to the run."""
    if not bases:
        fails.append("%s: ratio %s has no baseline column" % (where, ratio))
        return
    base = min(bases, key=lambda b: abs(ours / b - ratio))
    if abs(ours / base - ratio) > 0.006:
        fails.append("%s: %s/%s = %.4f, table says %s"
                     % (where, ours, base, ours / base, ratio))
        return
    if not runs_of(prefix):
        fails.append("%s: no results run for %s" % (where, prefix))
        return
    if not any(numbers_in(groups, case_bytes, op, ours, base)
               for _, groups in runs_of(prefix)):
        fails.append("%s: %s / %s not in %s (bytes %s op %s)"
                     % (where, ours, base,
                        ", ".join(os.path.basename(j) for j, _ in runs_of(prefix)),
                        case_bytes, op))
        return
    check_marker(where, ratio, marker)


def check_marker(where, ratio, marker):
    if marker is None:
        return
    want = "✅" if float(ratio) >= 1.0 else "❌"
    if marker != want:
        fails.append("%s: ratio %s carries %s, want %s"
                     % (where, ratio, marker, want))


def check_summary(prefix, case_bytes, ours, ratio, marker, where,
                  op_hint=None):
    """Summary cell (no printed baseline): a same-case+op pair in the cited
    run must quotient to the printed ratio; the op must fit the column."""
    if not runs_of(prefix):
        fails.append("%s: no results run for %s" % (where, prefix))
        return
    if not any(pair_in(groups, case_bytes, ours, ratio, op_hint)
               for _, groups in runs_of(prefix)):
        fails.append("%s: %s (%sx) not in %s (bytes %s%s)"
                     % (where, ours, ratio,
                        ", ".join(os.path.basename(j) for j, _ in runs_of(prefix)),
                        case_bytes,
                        " op~%s" % op_hint if op_hint else ""))
        return
    check_marker(where, ratio, marker)


def check_ratio_exists(prefix, case_bytes, ratio, where, op_hint=None,
                       marker=None):
    """Ratio-only cell (picker, baselines column): some same-case+op pair
    in the cited run quotients to it."""
    if not runs_of(prefix):
        fails.append("%s: no results run for %s" % (where, prefix))
        return
    ok = False
    for _, groups in runs_of(prefix):
        for (b, op), vals in groups.items():
            if b != case_bytes:
                continue
            if op_hint == "encode" and "encode" not in op:
                continue
            if op_hint == "decode" and not ("decode" in op or "dict" in op):
                continue
            if any(abs(o / bval - ratio) <= 0.006
                   for i, o in enumerate(vals)
                   for j, bval in enumerate(vals) if i != j):
                ok = True
    if not ok:
        fails.append("%s: %sx not in %s (bytes %s%s)"
                     % (where, ratio,
                        ", ".join(os.path.basename(j) for j, _ in runs_of(prefix)),
                        case_bytes,
                        " op~%s" % op_hint if op_hint else ""))
        return
    check_marker(where, ratio, marker)


# ---------- (a) chart anchors ----------
sections = bc.collect_sections()
generated = {}
for path, idx, title, series, rows in sections:
    slug = bc.slug_of(title)
    generated.setdefault(slug, title.replace("`", ""))
    svg = os.path.join(CHART_DIR, "bench-%s.svg" % slug)
    if not os.path.isfile(svg):
        fails.append("%s: section %r -> missing %s"
                     % (os.path.relpath(path, ROOT), title, os.path.basename(svg)))
        continue
    with open(path, encoding="utf-8") as f:
        body = f.read()
    if "bench-%s.svg)" % slug not in body:
        fails.append("%s: section %r has no embedded bench-%s.svg link"
                     % (os.path.relpath(path, ROOT), title, slug))
        continue
    line_hit = any(l.lstrip().startswith("![") and "bench-%s.svg)" % slug in l
                   for l in body.splitlines())
    if not line_hit:
        fails.append("%s: section %r mentions bench-%s.svg but not as an image"
                     % (os.path.relpath(path, ROOT), title, slug))

with open(BENCH, encoding="utf-8") as f:
    blines = f.read().splitlines()
for i, line in enumerate(blines):
    m = re.match(r"###+ (.+)", line)
    if not m:
        continue
    slug = bc.slug_of(m.group(1))
    if slug not in generated:
        continue
    window = blines[i + 1:i + 7]
    if not any("Full table and method:" in w for w in window):
        continue  # a tier-table section: covered by the loop above
    j = i + 1
    while j < len(blines) and not blines[j].startswith("#"):
        j += 1
    body = blines[i:j]
    if not any(l.lstrip().startswith("![") and "bench-%s.svg)" % slug in l
               for l in body):
        fails.append("BENCHMARKS.md: stub %r has no embedded bench-%s.svg link"
                     % (m.group(1), slug))
print("check-bench-refs: (a) chart anchors: %d sections, %d problems"
      % (len(sections), len([f for f in fails])))

# ---------- (b) ORDER vs at-a-glance row order ----------
with open(os.path.join(ROOT, "scripts", "bench-compare.py"), encoding="utf-8") as f:
    cmp_src = f.read()
ORDER = ast.literal_eval(re.search(r"ORDER\s*=\s*(\[.*?\])", cmp_src, re.S).group(1))

sec = blines.index("## At a glance: every binding, one tier")
nxt = next(i for i in range(sec + 1, len(blines)) if blines[i].startswith("## "))
AAG_FIX = {"core dynamic": "core", "c structs": "c", "c++": "c",
           "c#": "csharp", "f#": "fsharp", "objective-c": "objc"}
seq = []
for line in blines[sec:nxt]:
    if not line.startswith("|") or line.startswith("|---") \
            or line.startswith("| binding"):
        continue
    cell = line.strip().strip("|").split("|")[0]
    key = cell.split("(")[0].split("`")[0].strip().lower()
    key = AAG_FIX.get(key, key)
    if not seq or seq[-1] != key:
        seq.append(key)
if seq == ORDER:
    print("check-bench-refs: (b) ORDER matches at-a-glance order (%d rows)" % len(seq))
else:
    # Single documented exception: Dart sits right after Node in the table
    # while ORDER lists it after PHP (pre-existing drift; normalize the row
    # or ORDER in D2 and drop this branch).
    moved = [x for x in ORDER if x != "dart"]
    moved.insert(moved.index("node") + 1, "dart")
    if seq == moved:
        notices.append("KNOWN-EXCEPTION (b): at-a-glance lists Dart after "
                       "Node, ORDER lists it after PHP; otherwise in sync "
                       "(%d rows)" % len(seq))
    else:
        fails.append("(b) at-a-glance order %s != ORDER %s" % (seq, ORDER))

# ---------- (c) ratios vs cited results/*.json ----------
have_results = bool(glob.glob(os.path.join(RESULTS, "bench-*.json")))
if not have_results:
    notices.append("SKIP (c): no results/*.json present (git-ignored); "
                   "run after a bench re-run")
    c_base = 0
else:
    c_base = len(fails)

    # binding dir -> results prefix (cpp mirrors c; embedded has no cited run)
    DIRS = {"c": "c", "clojure": "clojure", "csharp": "csharp", "dart": "dart",
            "elixir": "elixir", "erlang": "erlang", "fsharp": "fsharp",
            "go": "go", "java": "java", "julia": "julia", "kotlin": "kotlin",
            "lua": "lua", "node": "node", "objc": "objc", "php": "php",
            "python": "python", "r": "r", "ruby": "ruby", "rust": "rust",
            "scala": "scala", "swift": "swift", "zig": "zig"}
    RUN = {}  # prefix -> [(path, groups)] (core cites several re-runs)
    for d, prefix in sorted(DIRS.items()):
        rp = os.path.join(ROOT, "bindings", d, "README.md")
        jp, groups = json_groups(prefix, rp)
        if jp is None:
            fails.append("bindings/%s/README.md: no results run for %s" % (d, prefix))
        else:
            RUN[prefix] = [(jp, groups)]
    core_stamps = sorted(set(re.findall(
        r"results/bench-core-(20261124T[0-9]+Z)\.json", open(BENCH).read())))
    core_runs = []
    for stamp in core_stamps:
        jp = os.path.join(RESULTS, "bench-core-%s.json" % stamp)
        if not os.path.isfile(jp):
            fails.append("BENCHMARKS.md cites missing %s" % os.path.basename(jp))
            continue
        groups = {}
        for case in jload(jp)["cases"]:
            for row in case["rows"]:
                groups.setdefault((case["bytes"], row["op"]), []).append(row["mbps"])
        core_runs.append((jp, groups))
    if not core_runs:
        # fall back to newest core record so the check still runs
        jp, groups = json_groups("core")
        if jp is not None:
            core_runs = [(jp, groups)]
    RUN["core"] = core_runs

    def tier_rows(path):
        """Yield (line_no, bytes, op, row_cells) for payload/op tables."""
        out = []
        lines = open(path, encoding="utf-8").read().splitlines()
        in_tier = False
        for i, line in enumerate(lines):
            if line.startswith("|"):
                cells = [c.strip() for c in line.strip().strip("|").split("|")]
                low = [c.lower() for c in cells]
                if "payload" in low and "op" in low:
                    in_tier = True
                    continue
                if in_tier and "---" not in line and re.search(r"\d", cells[0]):
                    m = re.search(r"(\d+)", cells[0])
                    if m and len(cells) >= 3:
                        out.append((i + 1, int(m.group(1)), cells[1], cells))
            else:
                in_tier = False
        return out

    # binding README tier tables (+ the core table in BENCHMARKS.md)
    TIER_FILES = [os.path.join(ROOT, "bindings", d, "README.md") for d in DIRS] + [BENCH]
    for path in TIER_FILES:
        if os.path.basename(path) == "BENCHMARKS.md":
            prefix = None  # core table: resolved per-row below
        else:
            prefix = DIRS[os.path.basename(os.path.dirname(path))]
            if prefix not in RUN:
                continue
        for ln, b, op, cells in tier_rows(path):
            if prefix is None and b not in (132, 169, 2755, 85933):
                continue  # not the core table (docker/embedded tables differ)
            use = "core" if prefix is None else prefix
            if use not in RUN:
                continue
            base_vals = []
            for cc in cells[3:]:
                base_vals += [float(x) for x in MBPS.findall(cc)]
            for rm in RATIO.finditer(cells[2]):
                mm = re.search(r"\d+\.\d+", cells[2])
                if not mm:
                    continue
                check_tier_row(use, b, op, float(mm.group(0)),
                               float(rm.group(1)), rm.group(2), base_vals,
                               "%s:%d" % (os.path.relpath(path, ROOT), ln))

    # at-a-glance + baselines tables + stub one-liners + picker
    AAG = blines[sec:nxt]
    for line in AAG:
        if not line.startswith("|") or line.startswith("|---") \
                or line.startswith("| binding"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        key = cells[0].split("(")[0].split("`")[0].strip().lower()
        key = AAG_FIX.get(key, key)
        if key not in RUN:
            fails.append("at-a-glance: no run mapping for %r" % cells[0])
            continue
        bm = re.search(r"(\d+) B", cells[2])
        if not bm:
            continue
        case_bytes = int(bm.group(1))
        for col, hint in ((3, "encode"), (4, "decode")):
            if col >= len(cells):
                continue
            for rm in RATIO.finditer(cells[col]):
                mm = re.search(r"\d+\.\d+", cells[col])
                check_summary(key, case_bytes, float(mm.group(0)),
                              float(rm.group(1)), rm.group(2),
                              "at-a-glance %s" % key, hint)

    # "Bindings vs their baselines" xlarge column: ratios only, no MB/s.
    # Parenthesized asides quote other tiers (prose-level, D3 owns those);
    # the column contract is the bare encode/decode pair.
    bsec = blines.index("## Bindings vs their baselines")
    bnx = next(i for i in range(bsec + 1, len(blines)) if blines[i].startswith("## "))
    for line in blines[bsec:bnx]:
        if not line.startswith("|") or line.startswith("|---") \
                or line.startswith("| binding |"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 5:
            continue  # the runtime/entry-points key, not the xlarge table
        key = cells[0].split("(")[0].strip().lower()
        key = {"core": "core", "c structs": "c", "c++": "c",
               "erlang/otp nif": "erlang", "c#": "csharp",
               "f#": "fsharp", "objective-c": "objc"}.get(key, key.split()[0])
        if key not in RUN:
            continue
        case_bytes = 85933 if key == "core" else 74913
        bare = re.sub(r"\(.*?\)", "", cells[4])
        parts = bare.split("/")
        for n, part in enumerate(parts[:2]):
            hint = "encode" if n == 0 else "decode"
            for rm in RATIO.finditer(part):
                marker = rm.group(2)
                if marker is None:
                    allm = re.findall(r"(✅|❌)", cells[4])
                    marker = allm[-1] if allm else None
                check_ratio_exists(key, case_bytes, float(rm.group(1)),
                                   "baselines-table %s %s" % (key, hint),
                                   hint, marker)

    # stub `xlarge:` one-liners: section -> binding via its README link
    STUB = re.compile(r"xlarge:\s*encode\s*(\d+\.\d+)\s*\((\d+\.\d+)x\s*(✅|❌)?\)"
                      r"\s*/\s*decode\s*(\d+\.\d+)\s*\((\d+\.\d+)x\s*(✅|❌)?\)")
    cur = []  # (line_no, text) of current ### section body
    def flush_stub(section):
        body = "\n".join(t for _, t in section)
        dm = re.search(r"bindings/([a-z0-9+#]+)/", body)
        if not dm:
            return
        prefix = {"cpp": "c"}.get(dm.group(1), dm.group(1))
        if prefix not in RUN:
            return
        for ln, text in section:
            m = STUB.search(text)
            if not m:
                continue
            case_bytes = 85933 if prefix == "core" else 74913
            check_summary(prefix, case_bytes, float(m.group(1)),
                          float(m.group(2)), m.group(3),
                          "BENCHMARKS.md:%d xlarge encode" % ln, "encode")
            check_summary(prefix, case_bytes, float(m.group(4)),
                          float(m.group(5)), m.group(6),
                          "BENCHMARKS.md:%d xlarge decode" % ln, "decode")
    for i, line in enumerate(blines):
        if re.match(r"###+ ", line):
            flush_stub(cur)
            cur = []
        elif line.startswith("## "):
            flush_stub(cur)
            cur = []
            continue
        cur.append((i + 1, line))
    flush_stub(cur)

    # top-level README picker table (ratios only, no MB/s, no markers)
    rlines = open(TOP_README, encoding="utf-8").read().splitlines()
    for i, line in enumerate(rlines):
        if not line.startswith("| ["):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        dm = re.search(r"bindings/([a-z0-9+#]+)/", cells[0])
        if not dm or len(cells) < 3:
            continue
        d = dm.group(1)
        prefix = {"cpp": "c"}.get(d, d)
        if prefix not in RUN or "(xlarge)" not in cells[2]:
            continue
        # KNOWN-EXCEPTION (c): the Erlang picker row still quotes the 20261123
        # run (3.00x/1.52x); every other Erlang table quotes the formal
        # 20261124 run (2.95x/1.59x). Refresh the picker; then drop this.
        if d == "erlang":
            notices.append("KNOWN-EXCEPTION (c): README picker Erlang row "
                           "quotes the 20261123 run (3.00x/1.52x), not the "
                           "formal 20261124 run (2.95x/1.59x)")
            continue
        case_bytes = 85933 if prefix == "core" else 74913
        # strip parenthesized asides quoting other tiers (e.g. Clojure's
        # "0.05x encode at 136 B"); the cell contract is the xlarge pair.
        bare = re.sub(r"\(.*?\)", "", cells[2])
        parts = bare.split("/")
        for n, part in enumerate(parts[:2]):
            hint = "encode" if n == 0 else "decode"
            for rm in RATIO.finditer(part):
                check_ratio_exists(prefix, case_bytes, float(rm.group(1)),
                                   "README picker %s %s" % (d, hint), hint)

    print("check-bench-refs: (c) ratios vs results: %d new problems"
          % (len(fails) - c_base))

print()
for n in notices:
    print(n)
if fails:
    print("\nFAIL (%d):" % len(fails))
    for f in fails:
        print("  " + f)
    sys.exit(1)
print("\ncheck-bench-refs: OK")
PYEOF
