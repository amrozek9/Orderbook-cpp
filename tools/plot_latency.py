#!/usr/bin/env python3
"""Plot bench --dump output as SVG: a latency histogram and a percentile curve.

    ./build-rel/bench --system-alloc --dump build-rel/malloc.csv
    ./build-rel/bench --dump build-rel/arena.csv
    python3 tools/plot_latency.py --out docs/latency \\
        "system malloc=build-rel/malloc.csv" "node arena=build-rel/arena.csv"

writes docs/latency-histogram.svg and docs/latency-percentiles.svg. Each
series is LABEL=FILE, drawn in the order given. The noise floor -- the bench's
"(spin)" samples, which time the machine alone -- is taken from the first file
and drawn in gray.

The histogram is log-log on purpose. On linear axes, the body of the
distribution is a spike near 100 ns and the tail is invisible; the tail is the
whole point. Bins are equal widths in log space, snapped to the clock's
resolution so that no bin straddles a quantization step, and heights are
normalized by bin width so that bins of different widths stay comparable.

Standard library only, so the figures regenerate anywhere Python 3 runs. The
SVGs carry their own light and dark palettes and follow the viewer's setting.
"""
import argparse
import bisect
import math
import sys

BINS_PER_DECADE = 12
LIGHT = {
    "surface": "#fcfcfb", "ink": "#0b0b0b", "ink2": "#52514e", "muted": "#898781",
    "grid": "#e1e0d9", "axis": "#c3c2b7", "s1": "#2a78d6", "s2": "#eb6834",
}
DARK = {
    "surface": "#1a1a19", "ink": "#ffffff", "ink2": "#c3c2b7", "muted": "#898781",
    "grid": "#2c2c2a", "axis": "#383835", "s1": "#3987e5", "s2": "#d95926",
}
FONT = 'system-ui, -apple-system, "Segoe UI", sans-serif'

W, H = 880, 500
LEFT, RIGHT, TOP, BOTTOM = 76, 24, 152, 56      # Plot margins


# --- data ---------------------------------------------------------------------

def load(path, kind, run):
    """Engine samples (optionally one kind / one run) and noise-floor samples."""
    ops, spin = [], []
    want_run = None if run is None else str(run)
    with open(path) as f:
        if next(f).strip() != "run,index,kind,ns":
            sys.exit(f"{path}: not a bench --dump file")
        for line in f:                      # Millions of rows: split, not csv
            r, _, k, ns = line.rstrip("\n").split(",")
            if want_run is not None and r != want_run:
                continue
            if k == "spin":
                spin.append(float(ns))
            elif kind == "all" or k == kind:
                ops.append(float(ns))
    if not ops:
        sys.exit(f"{path}: no samples for kind '{kind}'")
    ops.sort()
    spin.sort()
    return ops, spin


def percentile(sorted_ns, p):
    """Nearest rank, as bench reports it."""
    rank = math.ceil(p / 100 * len(sorted_ns))
    return sorted_ns[max(rank, 1) - 1]


def quantum(sorted_ns):
    """The clock's resolution, as the smallest gap between distinct values."""
    distinct = sorted({v for v in sorted_ns[: min(len(sorted_ns), 200_000)] if v > 0})
    gaps = [b - a for a, b in zip(distinct, distinct[1:]) if b - a > 0.5]
    return min(gaps) if gaps else 1.0


def bin_edges(q, lo, hi):
    """Log-spaced edges, snapped to half-quanta so each quantized value sits
    inside a bin rather than on its edge. Duplicates collapse, so the lowest
    bins are one clock step wide."""
    edges = []
    k = math.floor(math.log10(lo) * BINS_PER_DECADE)
    while True:
        e = 10 ** (k / BINS_PER_DECADE)
        snapped = (round(e / q - 0.5) + 0.5) * q
        if not edges or snapped > edges[-1]:
            edges.append(snapped)
        if e > hi:
            return edges
        k += 1


def histogram(sorted_ns, edges):
    """Share of samples per bin, scaled to a nominal 1/BINS_PER_DECADE decade."""
    n = len(sorted_ns)
    out = []
    for a, b in zip(edges, edges[1:]):
        count = bisect.bisect_left(sorted_ns, b) - bisect.bisect_left(sorted_ns, a)
        width = math.log10(b / a) * BINS_PER_DECADE
        out.append((a, b, count / n / width))
    below = bisect.bisect_left(sorted_ns, edges[0])         # Zero after overhead subtraction
    if below:
        a, b, d = out[0]
        out[0] = (a, b, d + below / n / (math.log10(b / a) * BINS_PER_DECADE))
    return out


# --- svg ----------------------------------------------------------------------

def fmt_ns(ns):
    if ns >= 1e6:
        return f"{ns / 1e6:g} ms"
    if ns >= 1e3:
        return f"{ns / 1e3:g} µs"
    return f"{ns:g} ns"


def fmt_int(ns):
    return f"{round(ns):,}"


def style():
    def block(p):
        return "".join(f"--{k}:{v};" for k, v in p.items())
    return f"""<style>
svg {{ {block(LIGHT)} font-family: {FONT}; }}
@media (prefers-color-scheme: dark) {{ svg {{ {block(DARK)} }} }}
.bg {{ fill: var(--surface); }}
.title {{ fill: var(--ink); font-size: 17px; font-weight: 600; }}
.sub {{ fill: var(--ink2); font-size: 13px; }}
.tick {{ fill: var(--muted); font-size: 12px; font-variant-numeric: tabular-nums; }}
.label {{ fill: var(--ink2); font-size: 12px; }}
.legend {{ fill: var(--ink); font-size: 13px; }}
.legend-num {{ fill: var(--ink2); font-size: 13px; font-variant-numeric: tabular-nums; white-space: pre; }}
.note {{ fill: var(--muted); font-size: 12px; }}
.grid {{ stroke: var(--grid); stroke-width: 1; }}
.axis {{ stroke: var(--axis); stroke-width: 1; }}
.s1 {{ stroke: var(--s1); }} .s2 {{ stroke: var(--s2); }} .floor {{ stroke: var(--muted); }}
.f1 {{ fill: var(--s1); }} .f2 {{ fill: var(--s2); }} .ffloor {{ fill: var(--muted); }}
.line {{ fill: none; stroke-width: 2; stroke-linejoin: round; stroke-linecap: round; }}
.wash {{ opacity: 0.10; stroke: none; }}
.marker {{ stroke-width: 1; }}
</style>"""


class Plot:
    """A log-x, log-y plot area with the fixed chrome both figures share."""

    def __init__(self, x_lo, x_hi, y_lo, y_hi):
        self.x_lo, self.x_hi = math.log10(x_lo), math.log10(x_hi)
        self.y_lo, self.y_hi = math.log10(y_lo), math.log10(y_hi)
        self.parts = []

    def x(self, v):
        t = (math.log10(v) - self.x_lo) / (self.x_hi - self.x_lo)
        return LEFT + min(max(t, 0.0), 1.0) * (W - LEFT - RIGHT)

    def y(self, v):
        v = max(v, 10 ** self.y_lo)
        t = (math.log10(v) - self.y_lo) / (self.y_hi - self.y_lo)
        return H - BOTTOM - t * (H - TOP - BOTTOM)

    def add(self, s):
        self.parts.append(s)

    def notes(self, notes, x_of, y_of):
        """Muted text labels anchored at data coordinates."""
        for x, y, text in notes:
            self.add(f'<text class="note" x="{x_of(x):.1f}" y="{y_of(y):.1f}">{text}</text>')

    def header(self, title, subtitle, legend):
        self.add(f'<text class="title" x="{LEFT}" y="28">{title}</text>')
        self.add(f'<text class="sub" x="{LEFT}" y="48">{subtitle}</text>')
        y = 76
        for swatch, name, numbers in legend:
            self.add(swatch(LEFT, y))
            self.add(f'<text class="legend" x="{LEFT + 26}" y="{y + 4}">{name}</text>')
            self.add(f'<text class="legend-num" x="{LEFT + 190}" y="{y + 4}">{numbers}</text>')
            y += 22

    def svg(self):
        body = "\n".join(self.parts)
        return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
                f'width="{W}" height="{H}" role="img">\n{style()}\n'
                f'<rect class="bg" width="{W}" height="{H}" rx="8"/>\n{body}\n</svg>\n')


def line_swatch(cls):
    return lambda x, y: f'<line class="line {cls}" x1="{x}" x2="{x + 18}" y1="{y}" y2="{y}"/>'


def percent_label(v):
    p = v * 100
    return f"{p:.0f}%" if p >= 1 else f"{p:.10f}".rstrip("0") + "%"


def legend_rows(series, floor):
    rows = []
    for i, (name, ns) in enumerate(series):
        nums = "  ·  ".join(f"{label} {fmt_int(percentile(ns, p))}"
                            for label, p in (("p50", 50), ("p99", 99), ("p99.9", 99.9)))
        rows.append((line_swatch(f"s{i + 1}"), name, nums + " ns"))
    if floor:
        rows.append((line_swatch("floor"), "machine alone", "busy-wait of the same length, no engine"))
    return rows


def histogram_svg(series, floor, q, title, notes):
    everything = [v for _, ns in series for v in ns] + floor
    x_lo = 10 ** math.floor(math.log10(max(q, 1)))
    x_hi = 10 ** math.ceil(math.log10(max(everything)))
    edges = bin_edges(q, x_lo, x_hi)
    hists = [histogram(ns, edges) for _, ns in series]
    floor_hist = histogram(floor, edges) if floor else []
    densities = [d for h in hists + [floor_hist] for _, _, d in h if d > 0]
    y_lo = 10 ** math.floor(math.log10(min(densities)))
    plot = Plot(x_lo, x_hi, y_lo, 1.0)

    n = len(series[0][1])
    plot.header(title,
                f"Share of {n:,} operations per latency bin, both axes logarithmic. "
                "Vertical lines mark each series' p99.9.",
                legend_rows(series, bool(floor)))

    # Grid and ticks.
    decade = 1                              # The floor itself means "empty bin": no label
    while 10 ** (plot.y_lo + decade) <= 1.0 + 1e-12:
        v = 10 ** (plot.y_lo + decade)
        y = plot.y(v)
        plot.add(f'<line class="grid" x1="{LEFT}" x2="{W - RIGHT}" y1="{y:.1f}" y2="{y:.1f}"/>')
        plot.add(f'<text class="tick" x="{LEFT - 8}" y="{y + 4:.1f}" text-anchor="end">'
                 f'{percent_label(v)}</text>')
        decade += 1
    v = x_lo
    while v <= x_hi * 1.0001:
        x = plot.x(v)
        plot.add(f'<line class="grid" x1="{x:.1f}" x2="{x:.1f}" y1="{TOP}" y2="{H - BOTTOM}"/>')
        plot.add(f'<text class="tick" x="{x:.1f}" y="{H - BOTTOM + 18}" text-anchor="middle">'
                 f'{fmt_ns(v)}</text>')
        v *= 10
    plot.add(f'<line class="axis" x1="{LEFT}" x2="{W - RIGHT}" y1="{H - BOTTOM}" y2="{H - BOTTOM}"/>')
    plot.add(f'<text class="label" x="{(LEFT + W - RIGHT) / 2}" y="{H - 14}" text-anchor="middle">'
             'latency of one operation</text>')

    def steps(hist):
        base = plot.y(y_lo)
        pts = [(plot.x(hist[0][0]), base)]
        for a, b, d in hist:
            y = plot.y(d) if d > 0 else base
            pts += [(plot.x(a), y), (plot.x(b), y)]
        pts.append((plot.x(hist[-1][1]), base))
        return pts

    def path(pts):
        return "M" + " L".join(f"{x:.1f},{y:.1f}" for x, y in pts)

    if floor_hist:
        plot.add(f'<path class="line floor" d="{path(steps(floor_hist))}"/>')
    for i, hist in enumerate(hists):
        pts = steps(hist)
        plot.add(f'<path class="wash f{i + 1}" d="{path(pts)} Z"/>')
    for i, hist in enumerate(hists):
        plot.add(f'<path class="line s{i + 1}" d="{path(steps(hist))}"/>')
    for i, (_, ns) in enumerate(series):
        x = plot.x(percentile(ns, 99.9))
        plot.add(f'<line class="marker s{i + 1}" x1="{x:.1f}" x2="{x:.1f}" '
                 f'y1="{TOP}" y2="{H - BOTTOM}"/>')
    plot.notes(notes, plot.x, plot.y)
    return plot.svg()


def nines(p):
    """Percentile to the x axis of the percentile plot: p90 -> 1, p99 -> 2 ..."""
    return -math.log10(1 - p / 100)


def percentiles_svg(series, floor, title, notes):
    n = len(series[0][1])
    top_nines = math.log10(n)
    everything = [v for _, ns in series for v in ns] + floor
    y_lo = 10 ** math.floor(math.log10(max(min(v for v in everything if v > 0), 1)))
    y_hi = 10 ** math.ceil(math.log10(max(everything)))
    # x is "nines": 1 at p90, 2 at p99, 3 at p99.9... offset so p0 sits at 1.
    plot = Plot(1, 10 ** (top_nines + 0.0), y_lo, y_hi)
    plot.x_lo, plot.x_hi = 0.0, top_nines

    def px(nines):
        return LEFT + nines / top_nines * (W - LEFT - RIGHT)

    plot.header(title,
                "Latency at each percentile, on a scale that gives every extra nine the same width.",
                legend_rows(series, bool(floor)))

    v = y_lo
    while v <= y_hi * 1.0001:
        y = plot.y(v)
        plot.add(f'<line class="grid" x1="{LEFT}" x2="{W - RIGHT}" y1="{y:.1f}" y2="{y:.1f}"/>')
        plot.add(f'<text class="tick" x="{LEFT - 8}" y="{y + 4:.1f}" text-anchor="end">{fmt_ns(v)}</text>')
        v *= 10
    for label, at in (("p50", math.log10(2)), ("p90", 1), ("p99", 2), ("p99.9", 3),
                      ("p99.99", 4), ("p99.999", 5), ("p99.9999", 6)):
        if at > top_nines:
            break
        x = px(at)
        plot.add(f'<line class="grid" x1="{x:.1f}" x2="{x:.1f}" y1="{TOP}" y2="{H - BOTTOM}"/>')
        plot.add(f'<text class="tick" x="{x:.1f}" y="{H - BOTTOM + 18}" text-anchor="middle">{label}</text>')
    plot.add(f'<line class="axis" x1="{LEFT}" x2="{W - RIGHT}" y1="{H - BOTTOM}" y2="{H - BOTTOM}"/>')
    plot.add(f'<text class="label" x="{(LEFT + W - RIGHT) / 2}" y="{H - 14}" text-anchor="middle">'
             'percentile (max at the right edge)</text>')

    def curve(ns):
        pts, steps = [], 600
        for i in range(steps + 1):
            at = top_nines * i / steps
            p = 100 * (1 - 10 ** -at)
            pts.append((px(at), plot.y(max(percentile(ns, p) if p > 0 else ns[0], y_lo))))
        return "M" + " L".join(f"{x:.1f},{y:.1f}" for x, y in pts)

    if floor:
        plot.add(f'<path class="line floor" d="{curve(floor)}"/>')
    for i, (_, ns) in enumerate(series):
        plot.add(f'<path class="line s{i + 1}" d="{curve(ns)}"/>')
    plot.notes(notes, lambda p: px(nines(p)), plot.y)
    return plot.svg()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("series", nargs="+", help="LABEL=FILE, in drawing order (at most two)")
    ap.add_argument("--out", required=True, help="output prefix: PREFIX-histogram.svg, PREFIX-percentiles.svg")
    ap.add_argument("--kind", default="all", help="all, add, cancel, modify or marketable")
    ap.add_argument("--run", type=int, help="use only this run from each file")
    ap.add_argument("--title", default="Latency per operation")
    ap.add_argument("--no-floor", action="store_true", help="omit the machine noise floor")
    ap.add_argument("--note", action="append", default=[], metavar="NS,SHARE,TEXT",
                    help="label on the histogram at a latency (ns) and share (0-1); repeatable")
    ap.add_argument("--pnote", action="append", default=[], metavar="PERCENTILE,NS,TEXT",
                    help="label on the percentile plot, e.g. 99.9,2000,text; repeatable")
    args = ap.parse_args()
    if len(args.series) > 2:
        sys.exit("at most two series: a before and an after")

    series, floor = [], []
    for spec in args.series:
        name, _, path = spec.partition("=")
        if not path:
            sys.exit(f"expected LABEL=FILE, got '{spec}'")
        ops, spin = load(path, args.kind, args.run)
        series.append((name, ops))
        if not floor and not args.no_floor:
            floor = spin

    def parse_notes(specs):
        out = []
        for spec in specs:
            x, y, text = spec.split(",", 2)
            out.append((float(x), float(y), text))
        return out
    notes, pnotes = parse_notes(args.note), parse_notes(args.pnote)

    q = quantum(series[0][1])
    title = args.title if args.kind == "all" else f"{args.title}: {args.kind}"
    for suffix, svg in (("histogram", histogram_svg(series, floor, q, title, notes)),
                        ("percentiles", percentiles_svg(series, floor, title, pnotes))):
        out = f"{args.out}-{suffix}.svg"
        with open(out, "w") as f:
            f.write(svg)
        print(f"wrote {out}")
    for name, ns in series:
        print(f"  {name:>16}: " + "  ".join(
            f"p{p:g} {fmt_int(percentile(ns, p))}" for p in (50, 99, 99.9, 99.99)) +
              f"  max {fmt_int(ns[-1])} ns  ({len(ns):,} samples)")


if __name__ == "__main__":
    main()
