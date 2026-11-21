"""Runs the pure-Python core's test modules (no framework, plain asserts).

    python3 bindings/python/tests/pure/run.py

Each module also runs standalone (`python3 .../test_wire.py`), and every test
is a plain `test_*` function so `pytest -q bindings/python/tests/pure` works too.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import test_codec  # noqa: E402
import test_descriptor  # noqa: E402
import test_wire  # noqa: E402

MODULES = [test_descriptor, test_wire, test_codec]


def main():
    failed = 0
    for module in MODULES:
        failed += module.main()
    if failed:
        print("%d pure-core test(s) failed" % failed)
    else:
        print("pure core: all tests passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
