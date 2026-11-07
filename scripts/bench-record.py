#!/usr/bin/env python3
"""Turn a pbwire benchmark log into one structured JSON record.

Every bench harness prints the same text blocks (see BENCHMARKS.md "Output
format")::

    <case>: <bytes> bytes, <iterations> iterations (repeat field x<repeats>)
      impl      op           ns/op       MB/s
      <impl>    <op>          ...        ...

This script parses those blocks, collects the machine / toolchain facts
needed to reproduce the run, and writes a single JSON file. The full log
stays next to it for performance debugging.

Usage:
    bench-record.py --binding NAME --log LOG --out OUT [--status N]
                    [--elapsed S] [--extra k=v ...]
    bench-record.py --selftest        # assert-based check of the parser

Only the standard library is used. Any collection step may fail (unknown
OS, missing tool); it records null and moves on, never aborting the run.
Schema version 1: fields are only ever added, never renamed.
"""

import argparse
import json
import os
import platform
import re
import subprocess
import sys
from datetime import datetime, timezone

SCHEMA_VERSION = 1

# "<case>: <bytes> bytes, ... <iterations> iterations" — the "..." covers
# every harness dialect ("floor N iterations" in Java, per-op silent counts
# in Python, which prints no iterations at all and no closing paren).
# Text/json/stream/threads tables of bench.cc never match: their byte
# counts are not directly after the colon.
TIER_RE = re.compile(
    r"^([A-Za-z0-9_.\-]+):\s+(\d+)\s+bytes,(?:\s+.*?(\d+)\s+iterations\b)?.*$"
)
REPEATS_RE = re.compile(r"[xX](\d+)\)?\s*$")
# A result row, parsed from the right: the last two fields are ns/op and
# MB/s, the field before is the op (one token), everything before that is
# the impl name (which may contain spaces: "pb gen", "pb upb").
ROW_RE = re.compile(
    r"^\s{2,}(.+?)\s+(\S+)\s+(\d+(?:\.\d+)?)\s+(\d+(?:\.\d+)?)\s*$"
)


def parse_log(text):
    """Return a list of case blocks in log order.

    A repeated case label (the PHP harness runs up to three passes with
    different baselines) starts a new block; nothing is merged.
    """
    cases = []
    current = None
    for line in text.splitlines():
        m = TIER_RE.match(line)
        if m:
            repeats = REPEATS_RE.search(line)
            current = {
                "case": m.group(1),
                "bytes": int(m.group(2)),
                "iterations": int(m.group(3)) if m.group(3) else None,
                "repeats": int(repeats.group(1)) if repeats else None,
                "rows": [],
            }
            cases.append(current)
            continue
        if current is None:
            continue
        # A non-indented line (OK gates, text:/json:/stream:/threads:
        # tables, blank-line-separated prose) ends the block, so rows of
        # a following table never leak into the previous case.
        if line.strip() == "" or not line[:1].isspace():
            current = None
            continue
        m = ROW_RE.match(line)
        if m:
            current["rows"].append(
                {
                    "impl": m.group(1).strip(),
                    "op": m.group(2),
                    "ns": float(m.group(3)),
                    "mbps": float(m.group(4)),
                }
            )
    return cases


def _run(cmd):
    try:
        out = subprocess.run(
            cmd, capture_output=True, text=True, timeout=10
        )
        if out.returncode == 0:
            return out.stdout.strip() or None
    except Exception:
        pass
    return None


def _cpu():
    system = platform.system()
    if system == "Darwin":
        model = _run(["sysctl", "-n", "machdep.cpu.brand_string"])
        return model or _run(["sysctl", "-n", "hw.model"])
    if system == "Linux":
        try:
            with open("/proc/cpuinfo") as f:
                for line in f:
                    if line.startswith("model name"):
                        return line.split(":", 1)[1].strip()
        except OSError:
            pass
    return None


def _in_container():
    """Docker/podman marker: the CPU and memory facts are then the VM's."""
    return os.path.exists("/.dockerenv") or os.path.exists("/run/.containerenv")


def _mem_mb():
    system = platform.system()
    if system == "Darwin":
        raw = _run(["sysctl", "-n", "hw.memsize"])
        if raw and raw.isdigit():
            return int(raw) // (1024 * 1024)
    if system == "Linux":
        try:
            with open("/proc/meminfo") as f:
                for line in f:
                    if line.startswith("MemTotal:"):
                        return int(line.split()[1]) // 1024
        except (OSError, ValueError, IndexError):
            pass
    return None


def collect_env(root):
    """Best-effort host facts; every field may be null."""
    uname = platform.uname()
    git_sha = _run(["git", "-C", root, "rev-parse", "--short", "HEAD"])
    dirty = None
    if git_sha is not None:
        tracked = _run(["git", "-C", root, "status", "--porcelain",
                        "--untracked-files=no"])
        dirty = (tracked or "") != "" if tracked is not None else None
    cc = _run(["cc", "--version"])
    return {
        "timestamp": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "git": {"sha": git_sha, "dirty": dirty},
        "host": {
            "os": "%s %s" % (uname.system, uname.release),
            "arch": uname.machine,
            "cpu": _cpu(),
            "cores": os.cpu_count(),
            "mem_mb": _mem_mb(),
            "container": _in_container(),
        },
        "toolchain": {
            "protoc": _run(["protoc", "--version"]),
            "cc": cc.splitlines()[0] if cc else None,
        },
    }


def build_record(binding, log_text, command, status, elapsed, extra):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    record = {"version": SCHEMA_VERSION, "binding": binding}
    record.update(collect_env(root))
    record["command"] = command
    record["status"] = status
    record["elapsed_s"] = elapsed
    record["extra"] = extra or {}
    record["cases"] = parse_log(log_text)
    return record


def selftest():
    sample = """
small: 132 bytes, 1048576 iterations (repeat field x0)
  impl      op           ns/op       MB/s
  pbwire    encode          61.1     2160.7
  pb gen    encode          81.2     1625.3
medium: 174 bytes, floor 524288 iterations (repeat field x1)
  impl           op             ns/op       MB/s
  pb upb         encode(data)   582.6       298.9
  pbwire-py      to_dict(bytes) 1074.2      162.0
threads: 1 vs 8 (14 cores), 500 iterations per thread, something
  impl      op          1thr-mbps  perthread-mbps  aggregate-mbps  per-op  aggregate
  pbwire    encode         1925.9        1661.8       13294.2    1.11x     1.51x
"""
    cases = parse_log(sample)
    assert len(cases) == 2, cases
    assert cases[0]["case"] == "small" and cases[0]["bytes"] == 132
    assert cases[0]["iterations"] == 1048576 and cases[0]["repeats"] == 0
    assert cases[0]["rows"][1] == {
        "impl": "pb gen", "op": "encode", "ns": 81.2, "mbps": 1625.3,
    }, cases[0]["rows"]
    assert cases[1]["rows"][0]["impl"] == "pb upb"
    assert cases[1]["rows"][0]["op"] == "encode(data)"
    assert cases[1]["rows"][1]["op"] == "to_dict(bytes)"
    # Threads table rows must not leak in as results.
    assert all(r["impl"] != "pbwire encode" for c in cases for r in c["rows"])
    # Rows of a following text:/json:/stream: table belong to no case.
    leak = parse_log(
        "small: 10 bytes, 5 iterations (repeat field x0)\n"
        "  pbwire encode 1.0 10.0\n"
        "\n"
        "text: small (9 bytes of text), 5 iterations\n"
        "  pbwire print 2.0 5.0\n"
    )
    assert len(leak) == 1 and len(leak[0]["rows"]) == 1, leak
    # Header lines and prose never start a block.
    assert parse_log("  impl op ns/op MB/s\n  pbwire encode abc def\n") == []
    # Python dialect: no iteration count, no closing paren.
    py = parse_log(
        "large: 2469 bytes, repeat field x100\n"
        "  pbwire-py    encode(data)        5277.7      467.8\n"
        "  pb upb       encode(data)       32769.1       75.3\n"
    )
    assert len(py) == 1 and py[0]["iterations"] is None, py
    assert py[0]["repeats"] == 100 and len(py[0]["rows"]) == 2, py
    assert py[0]["rows"][1]["impl"] == "pb upb", py
    print("selftest: parser OK (%d sample blocks)" % len(cases))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binding", help="binding name, e.g. core, python")
    ap.add_argument("--log", help="bench stdout log to parse")
    ap.add_argument("--out", help="JSON record to write")
    ap.add_argument("--status", type=int, default=0, help="bench exit code")
    ap.add_argument("--elapsed", type=float, default=None,
                    help="wall seconds the bench took")
    ap.add_argument("--extra", action="append", default=[],
                    help="extra k=v facts (repeatable)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        selftest()
        return 0
    if not args.binding or not args.log or not args.out:
        ap.error("--binding, --log and --out are required")
    with open(args.log, encoding="utf-8", errors="replace") as f:
        log_text = f.read()
    extra = {}
    for item in args.extra:
        key, sep, value = item.partition("=")
        if sep:
            extra[key] = value
    record = build_record(args.binding, log_text, None, args.status,
                          args.elapsed, extra)
    record["log"] = os.path.basename(args.log)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(record, f, indent=2)
        f.write("\n")
    print("record: %d cases -> %s" % (len(record["cases"]), args.out))
    if not record["cases"]:
        print("warning: no benchmark blocks parsed", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
