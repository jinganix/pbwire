#!/usr/bin/env python3
"""Render bench record JSON (scripts/bench-record.py) as markdown.

Usage: bench-render.py FILE [FILE...] > snippet.md
One section per file: header with the reproduction facts, then one
table per case block, in log order.
"""

import json
import sys


def render(path):
    with open(path, encoding="utf-8") as f:
        r = json.load(f)
    git = r.get("git") or {}
    host = r.get("host") or {}
    tool = r.get("toolchain") or {}
    dirty = " (dirty)" if git.get("dirty") else ""
    print("## %s — %s" % (r.get("binding", "?"), r.get("timestamp", "?")))
    print()
    print("git %s%s, %s / %s, %s; protoc %s; command exit %s in %ss" % (
        git.get("sha"), dirty, host.get("os"), host.get("arch"),
        host.get("cpu"), tool.get("protoc"), r.get("status"),
        r.get("elapsed_s")))
    for block in r.get("cases", []):
        repeats = ("x%s" % block["repeats"]) if block.get("repeats") is not None else "?"
        print()
        print("### %s: %s bytes (%s iterations, %s)" % (
            block["case"], block["bytes"], block["iterations"], repeats))
        print()
        print("| impl | op | ns/op | MB/s |")
        print("|---|---|---|---|")
        for row in block["rows"]:
            print("| %s | %s | %.1f | %.1f |" % (
                row["impl"], row["op"], row["ns"], row["mbps"]))


def main(argv):
    if not argv:
        print("usage: bench-render.py FILE [FILE...]", file=sys.stderr)
        return 2
    for i, path in enumerate(argv):
        if i:
            print()
        render(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
