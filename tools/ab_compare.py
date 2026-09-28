#!/usr/bin/env python3
"""Compare two builds of the benchmark with interleaved single runs.

    python3 tools/ab_compare.py --pairs 11 before/bench build-rel/bench
    python3 tools/ab_compare.py "build-rel/bench --price-levels 0" build-rel/bench

Each side is a bench command, optionally with its own flags. The two are run
alternately, one run each per pair, so drift in the machine -- a noisy
neighbour, a thermal change -- lands on both sides equally instead of on
whichever ran second. Every run must time the same flow (the flow hash is
checked), and extra arguments after "--" go to both commands.

For each operation type and percentile the report gives the median across
pairs, the change, and how many pairs the after side won. That count is a sign
test: if the change did nothing, each pair is a coin flip, and p is the chance
of a result at least this lopsided. It is robust to the occasional disturbed
run that inflates a max-minus-min spread, and it asks only the question an A/B
comparison needs answered: does the after side win consistently?
"""
import argparse
import math
import re
import shlex
import statistics
import subprocess
import sys

KINDS = ["all", "add", "cancel", "modify", "marketable"]
COLUMNS = ["p50", "p99", "p99.9"]
ROW = re.compile(r"\s+(all|add|cancel|modify|marketable)\s+[\d,]+\s+(.*)")


def run(command, extra):
    argv = shlex.split(command) + ["--runs", "1"] + extra
    out = subprocess.run(argv, capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"{command}: exit {out.returncode}\n{out.stderr}")
    text = out.stdout
    flow = re.search(r"hash ([0-9a-f]{16})", text)
    table = {}
    for line in text.split("latency (ns), median")[1].splitlines():
        m = ROW.match(line)
        if m:
            table[m.group(1)] = [int(v.replace(",", "")) for v in m.group(2).split()[:3]]
    return flow.group(1) if flow else None, table


def sign_test(wins, losses):
    """Two-sided p for `wins` out of wins + losses fair coin flips; ties dropped."""
    n = wins + losses
    if n == 0:
        return 1.0
    k = max(wins, losses)
    tail = sum(math.comb(n, i) for i in range(k, n + 1)) / 2 ** n
    return min(1.0, 2 * tail)


def min_wins(n):
    """Fewest wins out of n (no ties) that reach p < 0.05."""
    return next((w for w in range((n + 1) // 2, n + 1) if sign_test(w, n - w) < 0.05), n + 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("before", help="bench command for the baseline")
    ap.add_argument("after", help="bench command for the change")
    ap.add_argument("--pairs", type=int, default=11)
    ap.add_argument("extra", nargs="*", help="after --: arguments for both commands")
    args = ap.parse_args()

    results = []
    flow = None
    for i in range(args.pairs):
        pair = []
        for command in (args.before, args.after):
            h, table = run(command, args.extra)
            if flow is None:
                flow = h
            elif h != flow:
                sys.exit(f"{command}: timed flow {h}, expected {flow}; not comparable")
            pair.append(table)
        results.append(pair)
        print(f"  pair {i + 1}/{args.pairs}: all p99 {pair[0]['all'][1]} -> {pair[1]['all'][1]} ns",
              file=sys.stderr)

    n = len(results)
    print(f"{n} interleaved pairs, flow {flow}. Median across pairs, in ns, and how")
    print("many pairs the after side won. * marks p < 0.05 by a two-sided sign test,")
    print("in either direction: a consistent slowdown is marked as well.\n")
    print(f"{'':<11}" + "".join(f"{c:>28}" for c in COLUMNS))
    for kind in KINDS:
        line = f"{kind:<11}"
        for j in range(len(COLUMNS)):
            before = [r[0][kind][j] for r in results]
            after = [r[1][kind][j] for r in results]
            wins = sum(a < b for a, b in zip(after, before))
            losses = sum(a > b for a, b in zip(after, before))
            mb, ma = statistics.median(before), statistics.median(after)
            change = 100 * (ma - mb) / mb if mb else 0.0
            mark = "*" if sign_test(wins, losses) < 0.05 else " "
            line += f"{mb:>8,.0f} ->{ma:>6,.0f} {change:+4.0f}% {wins:>2}/{n}{mark}"
        print(line)
    need = min_wins(n)
    if need > n:
        print(f"\nWith {n} pairs nothing can reach p < 0.05; use at least 6.")
    else:
        print(f"\nWith {n} pairs and no ties, * needs {need} or more wins. Fewer is not a")
        print("result: the change may be real, but this machine cannot tell it from noise.")

if __name__ == "__main__":
    main()
