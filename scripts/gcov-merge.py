#!/usr/bin/env python3
"""Merge gcov line counts from two runs over the same sources.

Usage: gcov-merge.py <dir-a> <dir-b> <out> <src...>
Each dir holds a gcov-produced <base>.c.gcov per source. Counts for the same
(file, line) are summed (##### counts as 0); the merged set is written to
<out> in .gcov format for one file per source, plus a "Lines executed"
summary on stdout matching the Makefile floor math (weighted over the
given sources only).
"""
import re
import os
import sys


def parse(path):
    rows = {}
    try:
        with open(path, errors="replace") as f:
            for line in f:
                m = re.match(
                    r"\s*(#####|-|[\d.]+[kMGTPEZY]?)\s*:\s*(\d+)\s*:(.*)$",
                    line,
                )
                if not m:
                    continue
                cnt, ln, code = m.group(1), int(m.group(2)), m.group(3)
                rows[ln] = (cnt, code)
    except FileNotFoundError:
        pass
    return rows


def num(cnt):
    cnt = cnt.strip()
    if cnt == "-" or cnt == "#####" or cnt == "":
        return 0
    m = re.match(r"([\d.]+)", cnt)
    return float(m.group(1)) if m else 0


def fmt(n):
    if n == int(n):
        return str(int(n))
    return str(n)


def main():
    args = sys.argv[1:]
    floor = 65.0
    if "--floor" in args:
        i = args.index("--floor")
        floor = float(args[i + 1])
        del args[i : i + 2]
    dir_a, dir_b, out, sources = args[0], args[1], args[2], args[3:]
    total_e = 0
    total_t = 0
    nfiles = 0
    for src in sources:
        base = os.path.basename(src)
        a = parse(os.path.join(dir_a, base + ".gcov"))
        b = parse(os.path.join(dir_b, base + ".gcov"))
        lines = sorted(set(a) | set(b))
        with open(os.path.join(out, base + ".gcov"), "w") as f:
            for ln in lines:
                ca, code_a = a.get(ln, ("-", ""))
                cb, code_b = b.get(ln, ("", ""))
                code = code_a if code_a != "" else code_b
                na, nb = num(ca), num(cb)
                if ca.strip() == "-" and cb.strip() == "-":
                    cnt = "-"
                else:
                    cnt = fmt(na + nb) if (na + nb) > 0 else "#####"
                f.write("%9s:%5d:%s\n" % (cnt, ln, code))
        # file summary from merged executable lines
        e = t = 0
        for ln in lines:
            ca, _ = a.get(ln, ("-", ""))
            cb, _ = b.get(ln, ("-", ""))
            if ca.strip() == "-" and cb.strip() == "-":
                continue
            t += 1
            if num(ca) + num(cb) > 0:
                e += 1
        total_e += e
        total_t += t
        nfiles += 1
    pct = 100 * total_e / total_t if total_t else 0
    print("lines=%.1f%% over %d files" % (pct, nfiles))
    return 0 if (nfiles == 0 or total_t == 0 or pct >= floor) else 1


if __name__ == "__main__":
    sys.exit(main())
