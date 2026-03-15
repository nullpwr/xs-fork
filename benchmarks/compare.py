#!/usr/bin/env python3
"""Compare current benchmark results against a committed baseline.

Exits non-zero if the xs jit/vm/interp timings regress by more than
the tolerance (env TOLERANCE, default 0.25 = 25 percent). Writes a
markdown report to stdout.

Usage: compare.py baseline.json results.json
"""
import json, os, sys


def main():
    if len(sys.argv) != 3:
        print("usage: compare.py BASELINE RESULTS", file=sys.stderr)
        sys.exit(2)

    with open(sys.argv[1]) as f:
        base = json.load(f)
    with open(sys.argv[2]) as f:
        cur = json.load(f)

    tol = float(os.environ.get("TOLERANCE", "0.25"))
    regressed = []

    def parse(v):
        try:
            return float(v)
        except (ValueError, TypeError):
            return None

    print("| bench | backend | baseline | current | delta |")
    print("|-------|---------|---------:|--------:|------:|")
    for name, row in base.get("results", {}).items():
        cur_row = cur.get("results", {}).get(name, {})
        for backend in ("interp", "vm", "jit"):
            b = parse(row.get(backend))
            c = parse(cur_row.get(backend))
            if b is None or c is None or b == 0:
                continue
            delta = (c - b) / b
            marker = " :boom:" if delta > tol else ""
            print(f"| {name} | {backend} | {b:.3f} | {c:.3f} | {delta:+.1%}{marker} |")
            if delta > tol:
                regressed.append((name, backend, b, c, delta))

    if regressed:
        print(f"\n{len(regressed)} regression(s) beyond +{tol:.0%}:")
        for name, backend, b, c, d in regressed:
            print(f"  {name} [{backend}]: {b:.3f}s -> {c:.3f}s ({d:+.1%})")
        sys.exit(1)


if __name__ == "__main__":
    main()
