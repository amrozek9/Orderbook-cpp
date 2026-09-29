#!/usr/bin/env python3
"""Rebuild every optimization pass from its own commit and time them side by side.

    python3 tools/journey.py                     # build, 15 rounds on both books, report
    python3 tools/journey.py --rounds 5 --book default
    python3 tools/journey.py --report-only       # re-read the last run's samples

Each pass is built from the commit that introduced it, with the same compiler
and CMake's Release flags. A rejected experiment is its parent commit plus a
patch from tools/journey/, so the code that lost is on record too. Nothing
from the working tree is used.

A round runs every build once, a single bench run each, and the order rotates
from round to round. Drift in the machine -- a noisy neighbour, a thermal
change -- therefore lands on every pass alike rather than on whichever ran
last. Every run must time the same flow; the flow hash is checked.

The report gives each pass's median across rounds, then each pass against the
one it was built on, paired by round: how many rounds it won and lost, with a
two-sided sign test. It needs Python 3, git, CMake, a C++20 compiler, and
network access once, for Catch2.
"""
import argparse
import math
import os
import re
import shlex
import statistics
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCHES = os.path.join(REPO, "tools", "journey")

#name, commit, patch, built on, label
PASSES = [
    ("baseline",   "0c9a8b7", None,                        None,        "Baseline: std::map + std::list"),
    ("pmr",        "0c9a8b7", "pmr-pool.patch",            "baseline",  "Rejected: std::pmr pool"),
    ("arena",      "4c94921", None,                        "baseline",  "Pooled allocation: node arena"),
    ("flatarray",  "53982c1", None,                        "arena",     "Flat price array"),
    ("slotpool",   "f9515c3", None,                        "flatarray", "Intrusive lists (slot pool)"),
    ("fibonacci",  "17d5a7b", "fibonacci-hash.patch",      "slotpool",  "Rejected: Fibonacci-hashed id index"),
    ("idindex",    "17d5a7b", None,                        "slotpool",  "Cache layout: flat id index"),
    ("hotslot",    "5112213", None,                        "idindex",   "Cache layout: 32-byte hot slot"),
    ("levels16",   "035eb4d", None,                        "hotslot",   "Cache layout: 16-byte levels"),
    ("sidespec",   "035eb4d", "side-specialization.patch", "levels16",  "Rejected: specialize on side"),
    ("hints",      "1284ccd", None,                        "levels16",  "Branch tuning: profiled hints"),
    ("branchless", "1284ccd", "branchless-unlink.patch",   "hints",     "Rejected: branchless unlink"),
    ("hugepages",  "e2aaec5", None,                        "hints",     "Huge pages"),
]
BOOKS = {"default": [], "deep": ["--depth", "200000", "--ops", "1000000"]}
KINDS = ["all", "add", "cancel", "modify", "marketable"]
COLUMNS = ["p50", "p99", "p99.9"]
ROW = re.compile(r"\s+(all|add|cancel|modify|marketable)\s+[\d,]+\s+(.*)")


def sh(argv, log):
    with open(log, "a") as f:
        if subprocess.run(argv, stdout=f, stderr=subprocess.STDOUT).returncode != 0:
            sys.exit(f"failed: {shlex.join(argv)}\nsee {log}")


def build(workdir):
    catch2 = []
    for name, commit, patch, _, _ in PASSES:
        src = os.path.join(workdir, name)
        log = os.path.join(workdir, name + ".log")
        subprocess.run(["rm", "-rf", src, log], check=True)
        os.makedirs(src)
        archive = subprocess.run(["git", "-C", REPO, "archive", commit], capture_output=True, check=True)
        subprocess.run(["tar", "-x", "-C", src], input=archive.stdout, check=True)
        if patch:
            sh(["patch", "-p1", "-d", src, "-i", os.path.join(PATCHES, patch)], log)
        sh(["cmake", "-S", src, "-B", os.path.join(src, "build"), "-DCMAKE_BUILD_TYPE=Release"] + catch2, log)
        sh(["cmake", "--build", os.path.join(src, "build"), "--target", "bench", "-j"], log)
        if not catch2:     #Fetch Catch2 once, then point every other build at it
            catch2 = [f"-DFETCHCONTENT_SOURCE_DIR_CATCH2={src}/build/_deps/catch2-src"]
        print(f"built {name} from {commit}" + (f" + {patch}" if patch else ""), file=sys.stderr)


def parse(text):
    table = {}
    for line in text.split("latency (ns), median")[1].splitlines():
        m = ROW.match(line)
        if m:
            table[m.group(1)] = [int(v.replace(",", "")) for v in m.group(2).split()[:3]]
    return table


def run(workdir, book, rounds):
    out = os.path.join(workdir, "raw", book)
    os.makedirs(out, exist_ok=True)
    flow = None
    for r in range(rounds):
        k = r % len(PASSES)
        for name, *_ in PASSES[k:] + PASSES[:k]:
            bench = os.path.join(workdir, name, "build", "bench")
            res = subprocess.run([bench, "--runs", "1"] + BOOKS[book], capture_output=True, text=True)
            if res.returncode != 0:
                sys.exit(f"{name}: exit {res.returncode}\n{res.stderr}")
            h = re.search(r"hash ([0-9a-f]{16})", res.stdout).group(1)
            if flow is None:
                flow = h
            elif h != flow:
                sys.exit(f"{name}: timed flow {h}, expected {flow}; not comparable")
            with open(os.path.join(out, f"{name}-{r + 1:02d}.txt"), "w") as f:
                f.write(res.stdout)
        print(f"{book}: round {r + 1}/{rounds}", file=sys.stderr)


def sign_test(wins, losses):
    """Two-sided p for `wins` out of wins + losses fair coin flips; ties dropped."""
    n = wins + losses
    if n == 0:
        return 1.0
    k = max(wins, losses)
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(k, n + 1)) / 2 ** n)


def report(workdir, book):
    raw = os.path.join(workdir, "raw", book)
    if not os.path.isdir(raw):
        return
    data = {}
    for name, *_ in PASSES:
        data[name] = {}
        for f in os.listdir(raw):
            if f.startswith(name + "-"):
                with open(os.path.join(raw, f)) as fh:
                    data[name][f[len(name) + 1:-4]] = parse(fh.read())
    rounds = sorted(set.intersection(*(set(d) for d in data.values())))
    if not rounds:
        return

    def med(name, kind, j):
        return statistics.median(data[name][r][kind][j] for r in rounds)

    print(f"{book} book, {len(rounds)} rounds: median across rounds, ns, all operations\n")
    print(f"{'':<38}" + "".join(f"{c:>8}" for c in COLUMNS))
    for name, _, _, _, label in PASSES:
        print(f"{label:<38}" + "".join(f"{med(name, 'all', j):>8,.0f}" for j in range(3)))
    print("\nEach pass against the one it was built on, paired by round: median before ->")
    print("after, and rounds won/lost. * marks p < 0.05 by a two-sided sign test.\n")
    for name, _, _, parent, label in PASSES[1:]:
        print(f"{label} (vs {parent})")
        for kind in KINDS:
            line = f"  {kind:<11}"
            for j in range(3):
                before = [data[parent][r][kind][j] for r in rounds]
                after = [data[name][r][kind][j] for r in rounds]
                wins = sum(a < b for a, b in zip(after, before))
                losses = sum(a > b for a, b in zip(after, before))
                mb, ma = statistics.median(before), statistics.median(after)
                mark = "*" if sign_test(wins, losses) < 0.05 else " "
                line += f"{mb:>7,.0f} ->{ma:>6,.0f} {100 * (ma - mb) / mb:+4.0f}% {wins:>2}/{losses:<2}{mark}"
            print(line)
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--rounds", type=int, default=15)
    ap.add_argument("--book", choices=["default", "deep", "both"], default="both")
    ap.add_argument("--workdir", default=os.path.join(REPO, "build-journey"))
    ap.add_argument("--report-only", action="store_true", help="report on the samples already in --workdir")
    args = ap.parse_args()
    books = list(BOOKS) if args.book == "both" else [args.book]
    if not args.report_only:
        os.makedirs(args.workdir, exist_ok=True)
        build(args.workdir)
        for book in books:
            subprocess.run(["rm", "-rf", os.path.join(args.workdir, "raw", book)], check=True)
            run(args.workdir, book, args.rounds)
    for book in books:
        report(args.workdir, book)


if __name__ == "__main__":
    main()
