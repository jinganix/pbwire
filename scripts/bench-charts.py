#!/usr/bin/env python3
"""Render BENCHMARKS.md tables as SVG bar charts (vertical bars).

Single source of truth: everything is parsed straight out of
BENCHMARKS.md, so the charts can never drift from the tables.

Every per-language tier table -> docs/charts/bench-<slug>.svg,
  embedded under its own section header (idempotent re-run)

Re-run after re-pasting any table:

    python3 scripts/bench-charts.py
"""
import math
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = os.path.join(ROOT, "BENCHMARKS.md")
OUT_DIR = os.path.join(ROOT, "docs", "charts")

RATIO = re.compile(r"\(([\d.]+)x\s*[✅❌]\)")
NUM = re.compile(r"[\d.]+")

GREEN = ["#2da44e", "#94d39e"]
GRAY = ["#57606a", "#8c959f", "#c9d1d9"]


def slug_of(title):
    return re.sub(r"[^a-z0-9]+", "-",
                  title.replace("`", "").lower()).strip("-")


def parse_section_tables(lines):
    """Map section header -> first tier table under it.

    Returns [(line_idx, title, series_names, rows)] where each row is
    (payload, op, [mb_or_none_per_series]). Only the first
    payload/op table per header is kept (e.g. PHP's C-extension table;
    the pure-PHP appendix stays table-only).
    """
    found, cur = [], None
    for i, line in enumerate(lines):
        if re.match(r"###+ ", line):
            cur = (i, line.strip("# ").strip())
        elif line.startswith("## "):
            cur = None
        elif cur and line.startswith("|"):
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            names = [c.lower() for c in cells]
            if "payload" in names and "op" in names:
                if any(f[1] == cur[1] for f in found):
                    cur = None  # second table under this header: skip
                    continue
                found.append([cur[0], cur[1], None, []])
    # second pass: fill bodies (simpler and robust)
    result = []
    for idx, title, _, _ in found:
        j = idx + 1
        while j < len(lines) and not lines[j].startswith("|"):
            j += 1
        header = [c.strip() for c in lines[j].strip().strip("|").split("|")]
        series = [h.split("(")[0].replace("`", "").strip() for h in header[2:]]
        rows = []
        for k in range(j + 1, len(lines)):
            if not lines[k].startswith("|"):
                break
            cells = [c.strip() for c in lines[k].strip().strip("|").split("|")]
            if len(cells) < 3 or not re.search(r"\d", cells[0]):
                continue
            vals = []
            for c in cells[2:2 + len(series)]:
                m = NUM.search(c.replace(",", ""))
                mb = float(m.group(0)) if m else None
                r = RATIO.search(c)
                st = ("✅" in r.group(0)) if r else None
                vals.append((mb, st))
            rows.append((cells[0], cells[1], vals))
        if rows and series:
            # op-major order: group by op (first-appearance order),
            # tiers ascending within each op (stable sort keeps table order)
            ops = []
            for _, op, _ in rows:
                if op not in ops:
                    ops.append(op)
            rows = sorted(rows, key=lambda r: ops.index(r[1]))
            result.append((idx, title, series, rows))
    return result


def log_ticks(lo, hi):
    ticks = []
    k = math.floor(math.log10(lo))
    while 10 ** k < hi:
        for m in (1, 2, 5):
            v = m * 10 ** k
            if lo * 0.999 <= v <= hi * 1.001:
                ticks.append(v)
        k += 1
    return ticks


def series_colors(series):
    gi, ri, colors = 0, 0, []
    for s in series:
        if "pbw" in s.lower() or s.startswith("pbwire"):
            colors.append(GREEN[gi % len(GREEN)])
            gi += 1
        else:
            colors.append(GRAY[ri % len(GRAY)])
            ri += 1
    return colors


def lang_svg(title, series, rows, path):
    """One vertical grouped-bar chart per language: group per tier/op."""
    n = len(series)
    BW, BGAP, GGAP = 24, 6, 20
    gw = n * (BW + BGAP) + GGAP
    LEFT, RIGHT, TOP, BOTTOM, H = 64, 24, 96, 72, 500
    W = LEFT + len(rows) * gw + RIGHT
    vals = [v for _, _, vs in rows for v, _ in vs if v]
    lo, hi = min(vals) / 1.15, max(vals) * 1.2
    ph, base = H - TOP - BOTTOM, H - BOTTOM

    def y(v):
        return TOP + (math.log10(hi) - math.log10(v)) / (
            math.log10(hi) - math.log10(lo)) * ph

    colors = series_colors(series)
    has_ratio = [any(rows[i][2][j][1] is not None for i in range(len(rows)))
                 for j in range(n)]
    has_red = [any(rows[i][2][j][1] is False for i in range(len(rows)))
               for j in range(n)]
    p = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" '
         f'height="{H}" font-family="Helvetica, Arial, sans-serif">',
         f'<rect width="{W}" height="{H}" fill="#ffffff"/>']
    p.append(f'<text x="{W/2}" y="26" text-anchor="middle" '
             f'font-size="15" font-weight="bold">{title} — MB/s by tier '
             f"(log scale)</text>")
    lx = LEFT
    for j, s in enumerate(series):
        if has_ratio[j] and has_red[j]:
            p.append(f'<rect x="{lx}" y="40" width="6" height="12" '
                     f'fill="#2da44e"/>')
            p.append(f'<rect x="{lx+6}" y="40" width="6" height="12" '
                     f'fill="#cf222e"/>')
        else:
            fill = colors[j] if not has_ratio[j] else "#2da44e"
            p.append(f'<rect x="{lx}" y="40" width="12" height="12" '
                     f'fill="{fill}"/>')
        p.append(f'<text x="{lx+16}" y="51" font-size="12">{s}</text>')
        lx += 16 + len(s) * 6.5 + 22
    for t in log_ticks(lo, hi):
        p.append(f'<line x1="{LEFT}" y1="{y(t):.1f}" x2="{W-RIGHT}" '
                 f'y2="{y(t):.1f}" stroke="#e1e4e8"/>')
        p.append(f'<text x="{LEFT-8}" y="{y(t)+4:.1f}" text-anchor="end" '
                 f'font-size="10" fill="#57606a">{t:g}</text>')
    for i, (payload, op, vs) in enumerate(rows):
        left = LEFT + i * gw + GGAP // 2
        cx = left + (n * (BW + BGAP) - BGAP) / 2
        p.append(f'<text x="{cx:.1f}" y="{base+20}" text-anchor="middle" '
                 f'font-size="10">{payload}</text>')
        p.append(f'<text x="{cx:.1f}" y="{base+34}" text-anchor="middle" '
                 f'font-size="10" fill="#57606a">{op}</text>')
        for j, (v, st) in enumerate(vs):
            if v is None:
                continue
            if has_ratio[j]:
                fill = "#2da44e" if st is not False else "#cf222e"
            else:
                fill = colors[j]
            bx = left + j * (BW + BGAP)
            p.append(f'<rect x="{bx:.1f}" y="{y(v):.1f}" width="{BW}" '
                     f'height="{base-y(v):.1f}" rx="2" fill="{fill}"/>')
            p.append(f'<text x="{bx+BW/2:.1f}" y="{y(v)-5:.1f}" '
                     f'text-anchor="middle" font-size="10" '
                     f'fill="#24292f">{v:g}</text>')
    p.append(f'<text x="{LEFT}" y="{H-10}" font-size="10" '
             f'fill="#57606a">green = ✅ beats baseline · '
             f'red = ❌ trails</text>')
    p.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(p) + "\n")


def main():
    with open(BENCH) as f:
        lines = f.read().splitlines()
    os.makedirs(OUT_DIR, exist_ok=True)
    sections = parse_section_tables(lines)
    embeds = {}  # header line idx -> chart markdown line
    for idx, title, series, srows in sections:
        slug = slug_of(title)
        path = os.path.join(OUT_DIR, f"bench-{slug}.svg")
        lang_svg(title.replace("`", ""), series, srows, path)
        print(f"wrote {path} ({len(srows)} rows x {len(series)} series)")
        plain = title.replace("`", "")
        embeds[idx] = f"![{plain} — MB/s by tier](docs/charts/bench-{slug}.svg)"

    # embed under each header (bottom-up so indices stay valid;
    # skip blank lines when looking for an existing chart line)
    changed = False
    for idx in sorted(embeds, reverse=True):
        j = idx + 1
        while j < len(lines) and lines[j] == "":
            j += 1
        if j < len(lines) and lines[j].startswith("![") \
                and "docs/charts/bench-" in lines[j]:
            if lines[j] != embeds[idx]:
                lines[j] = embeds[idx]
                changed = True
        else:
            lines[idx + 1:idx + 1] = ["", embeds[idx]]
            changed = True
    if changed:
        with open(BENCH, "w") as f:
            f.write("\n".join(lines) + "\n")
        print(f"embedded {len(embeds)} charts into BENCHMARKS.md")


if __name__ == "__main__":
    main()
