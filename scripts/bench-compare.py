#!/usr/bin/env python3
"""Compare benchmark records across platforms, row by row.

`scripts/bench-record.py` writes one JSON per harness run: the host facts plus
the parsed result rows. `make bench` runs on the host (macOS in BENCHMARKS.md)
and `make bench-docker` runs the same harnesses in a Linux container; this
script pairs their records per binding and prints, per case and op, the speedup
over the fastest baseline on each side (`pbwire*` rows vs everything else,
which is how BENCHMARKS.md reads `x baseline`).

    bench-compare.py --latest results      # newest record per binding/platform
    bench-compare.py --tier xlarge A.json B.json
    bench-compare.py --absolute ...        # also print the raw MB/s per row

Absolute MB/s is not comparable between machines (different CPU, and a
container adds virtualization), so the speedup is the column to read: the same
`x baseline` on both sides is what says whether a fast path holds on Linux too.

Only the standard library is used.
"""

import argparse
import glob
import json
import os
import sys


def platform_of(record):
    """Darwin reads as macOS, so the two column names form a pair."""
    name = ((record.get("host") or {}).get("os") or "?").split()[0]
    return "macOS" if name == "Darwin" else name


def rows_of(record):
    """{(case, block, impl, op): MB/s}; block counts repeated case labels.

    The PHP harness prints the same case twice (one pass per baseline), so the
    block index is part of the key: both sides run the same passes in order.
    """
    out = {}
    seen = {}
    for block in record.get("cases") or []:
        case = block.get("case")
        idx = seen.get(case, 0)
        seen[case] = idx + 1
        for row in block.get("rows") or []:
            out[(case, idx, row["impl"], row["op"])] = row["mbps"]
    return out


def load(paths):
    return [(path, json.load(open(path, encoding="utf-8"))) for path in paths]


def newest(pairs):
    """Newest record per (binding, platform), so runs can pile up."""
    best = {}
    order = []
    for path, record in pairs:
        key = (record.get("binding"), platform_of(record))
        stamp = record.get("timestamp") or ""
        if key in best and best[key][1].get("timestamp", "") >= stamp:
            continue
        if key not in best:
            order.append(key)
        best[key] = (path, record)
    return [best[key] for key in order]


def latest_per_binding(paths):
    return newest(load(paths))


def speedups(rows):
    """{(case, block, op): (speedup, baseline impl)} for one side.

    Ours are the `pbwire*` impl names, the baseline is the fastest other row
    for the same op -- the same "best of the official runtimes" the tables in
    BENCHMARKS.md use.
    """
    ours, base = {}, {}
    for (case, idx, impl, op), mbps in rows.items():
        key = (case, idx, op)
        if impl.startswith("pbwire"):
            ours[key] = max(ours.get(key, 0.0), mbps)
        elif mbps > base.get(key, (0.0, ""))[0]:
            base[key] = (mbps, impl)
    return {key: (ours[key] / base[key][0], base[key][1])
            for key in ours if key in base and base[key][0] > 0}


def pick_pair(records):
    """Two platforms: macOS first when present, else the first two seen."""
    platforms = []
    for _, record in records:
        name = platform_of(record)
        if name not in platforms:
            platforms.append(name)
    if not platforms:
        sys.exit("bench-compare: no records")
    if "macOS" in platforms:
        platforms.remove("macOS")
        platforms.insert(0, "macOS")
    if len(platforms) > 2:
        print("bench-compare: comparing %s, ignoring %s" % (
            " vs ".join(platforms[:2]), ", ".join(platforms[2:])), file=sys.stderr)
    return platforms[:2]


def describe(record):
    host = record.get("host") or {}
    tool = record.get("toolchain") or {}
    bits = [(host.get("os") or "?") + (" container" if host.get("container") else ""),
            host.get("arch") or "?", host.get("cpu") or "?",
            "%s cores" % host.get("cores"), tool.get("protoc") or "no protoc"]
    return "%s (%s)" % (record.get("timestamp") or "?", ", ".join(str(b) for b in bits))


def absolute_table(rows, left, right, keys):
    """Same rows on both sides, MB/s each -- only meaningful on one machine."""
    print("| case | impl | op | %s MB/s | %s MB/s | %s/%s |" % (
        left, right, right, left))
    print("|---|---|---|---|---|---|")
    for (case, idx, impl, op) in keys:
        a, b = rows[left].get((case, idx, impl, op)), rows[right].get((case, idx, impl, op))
        if a is None or b is None:
            continue
        ratio = b / a if a > 0 else 0.0
        print("| %s | %s | %s | %.1f | %.1f | %.2fx |" % (case, impl, op, a, b, ratio))
    print()


# Section order: the at-a-glance order of BENCHMARKS.md, then anything else.
ORDER = ["core", "c", "go", "python", "java", "kotlin", "node", "ruby", "objc",
         "php", "dart", "rust", "csharp", "lua", "erlang", "swift", "scala",
         "elixir", "zig", "fsharp", "clojure", "julia", "r"]


def order_key(binding):
    return (ORDER.index(binding) if binding in ORDER else len(ORDER), binding)


def compare(records, tier, absolute):
    """Markdown tables, one per binding, paired across platforms."""
    platforms = pick_pair(records)
    left = platforms[0]
    right = platforms[1] if len(platforms) > 1 else None

    by_binding = {}
    for path, record in records:
        name = platform_of(record)
        if name not in platforms:
            continue
        binding = record.get("binding") or os.path.basename(path)
        by_binding.setdefault(binding, {})[name] = record

    skipped = []
    for binding in sorted(by_binding, key=order_key):
        sides = by_binding[binding]
        missing = [p for p in platforms if p not in sides]
        if missing:
            skipped.append("%s (no %s record)" % (binding, ", ".join(missing)))
            continue
        rows = {name: rows_of(record) for name, record in sides.items()}
        print("### %s" % binding)
        print()
        print("- %s: %s" % (left, describe(sides[left])))
        if right:
            print("- %s: %s" % (right, describe(sides[right])))
        print()

        metrics = {name: speedups(rows[name]) for name in rows}
        pair = {key: (metrics[left][key], metrics[right].get(key) if right else None)
                for key in metrics[left]}
        if right:
            compared = {k: v for k, v in pair.items() if v[1] is not None}
            # Different baselines per side (PHP runs the pure-PHP runtime when
            # the C extension is missing) cannot be compared with each other.
            mixed = [k for k in compared if compared[k][0][1] != compared[k][1][1]]
            if mixed:
                print("skipped %d row(s): the baseline differs across sides (%s)\n"
                      % (len(mixed), ", ".join(sorted(set(
                          "%s vs %s" % (compared[k][0][1], compared[k][1][1])
                          for k in mixed)))))
            pair = {k: v for k, v in compared.items() if k not in mixed}
        if tier:
            pair = {key: v for key, v in pair.items() if key[0] == tier}
        if not pair:
            print("no comparable row%s\n" % (" for %s" % tier if tier else ""))
            continue
        passes = any(key[1] > 0 for key in pair)
        cols = ["case"] + (["pass"] if passes else []) + ["op", left]
        if right:
            cols.append(right)
        print("| %s |" % " | ".join(
            c if c in ("case", "pass", "op") else "%s x baseline" % c for c in cols))
        print("|%s" % ("---|" * len(cols)))
        for key in pair:
            case, idx, op = key
            cells = ["%.2fx (%s)" % pair[key][0]]
            if pair[key][1]:
                cells.append("%.2fx (%s)" % pair[key][1])
            row = [case] + (["%d" % (idx + 1)] if passes else []) + [op] + cells
            print("| %s |" % " | ".join(row))
        print()
        if absolute and right:
            keys = sorted(k for name in rows for k in rows[name])
            absolute_table(rows, left, right, keys)

    if skipped:
        # stdout stays paste-ready markdown for BENCHMARKS.md.
        print("bench-compare: no pair for %s" % ", ".join(skipped), file=sys.stderr)


def selftest():
    def record(binding, os_name, stamp, mbps, ours="pbwire"):
        rows = [{"impl": ours, "op": "encode", "ns": 1.0, "mbps": mbps},
                {"impl": "pb gen", "op": "encode", "ns": 1.0, "mbps": 100.0}]
        return {
            "binding": binding, "timestamp": stamp,
            "host": {"os": os_name, "arch": "x", "cpu": "c", "cores": 4},
            "cases": [{"case": "small", "bytes": 1, "iterations": 1,
                       "repeats": 0, "rows": rows}],
        }

    mac_a = record("core", "Darwin 27.0.0", "2026-01-01", 200.0)
    lin = record("core", "Linux 6.8", "2026-01-02", 150.0)
    mac_b = record("core", "Darwin 27.0.0", "2026-01-03", 210.0)
    # --latest keeps the newest macOS record, never the older one.
    picked = dict(((r.get("binding"), platform_of(r)), r.get("timestamp"))
                  for _, r in newest([("", mac_a), ("", lin), ("", mac_b)]))
    assert picked[("core", "macOS")] == "2026-01-03", picked
    assert picked[("core", "Linux")] == "2026-01-02", picked
    # Rows pair by (case, impl, op); the speedup is ours vs the fastest baseline.
    assert speedups(rows_of(mac_b))[("small", 0, "encode")] == (2.1, "pb gen")
    assert speedups(rows_of(lin))[("small", 0, "encode")][0] == 1.5
    # Repeated case labels stay separate blocks (the two PHP passes).
    twice = {"cases": [{"case": "small", "rows": [{"impl": "p", "op": "encode", "ns": 1, "mbps": 5.0}]},
                       {"case": "small", "rows": [{"impl": "p", "op": "encode", "ns": 1, "mbps": 6.0}]}]}
    assert rows_of(twice)[("small", 1, "p", "encode")] == 6.0
    assert platform_of({"host": {"os": "Linux 6.8"}}) == "Linux"
    assert platform_of({"host": {"os": "Darwin 27.0.0"}}) == "macOS"
    # A binding with no baseline row on one side has no speedup to report.
    assert speedups(rows_of(record("lua", "Linux 6.8", "x", 5.0,
                                   ours="other"))) == {}
    print("selftest: compare OK")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("files", nargs="*", help="bench record JSON files")
    ap.add_argument("--latest", metavar="DIR",
                    help="use the newest record per binding/platform under DIR")
    ap.add_argument("--tier", help="only this case label (e.g. xlarge)")
    ap.add_argument("--absolute", action="store_true",
                    help="also print the raw MB/s per row (same-machine only)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        selftest()
        return 0
    if args.latest:
        paths = sorted(glob.glob(os.path.join(args.latest, "bench-*.json")))
        records = latest_per_binding(paths)
    elif args.files:
        records = load(args.files)
    else:
        ap.error("pass record files or --latest DIR")
    if not records:
        ap.error("no bench records found")
    compare(records, args.tier, args.absolute)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
