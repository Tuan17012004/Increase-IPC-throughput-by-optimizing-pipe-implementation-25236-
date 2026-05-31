#!/usr/bin/env python3
"""
Compare two IPC throughput result files (baseline vs optimized) produced
by xv6 `pipebench`, and draw a side-by-side comparison chart.

Workflow:
    1. Inside xv6 (with BASELINE pipe.c):
           pipebench
           cat ipctp.txt
       Copy the output to host as e.g. baseline.txt.

    2. Apply the optimization, rebuild xv6, run again:
           pipebench
           cat ipctp.txt
       Copy the output to host as e.g. optimized.txt.

    3. On host:
           python3 tools/compare_throughput.py baseline.txt optimized.txt
       Produces compare.png with bar + line charts and prints a speed-up
       table to stdout.

Usage:
    python3 tools/compare_throughput.py BASELINE OPTIMIZED [-o output.png] [--show]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def parse_data(path: Path):
    header = ""
    points: list[tuple[int, int]] = []
    with path.open("r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            if line.startswith("#"):
                if not header:
                    header = line.lstrip("#").strip()
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                label = int(parts[0])
                value = int(parts[1])
            except ValueError:
                continue
            points.append((label, value))
    return header, points


def detect_xlabel(header: str, default: str = "x") -> str:
    if "xaxis=" in header:
        for token in header.split():
            if token.startswith("xaxis="):
                return token.split("=", 1)[1]
    return default


def align_points(base_points, opt_points):
    """Align two datasets by their x-label so we can compare directly."""
    base_map = dict(base_points)
    opt_map = dict(opt_points)
    common = sorted(set(base_map) & set(opt_map))
    xs = common
    ys_base = [base_map[x] for x in xs]
    ys_opt = [opt_map[x] for x in xs]
    return xs, ys_base, ys_opt


def print_table(xs, ys_base, ys_opt, xlabel: str) -> None:
    print(f"\n{'='*60}")
    print(f"{xlabel:>12}  {'baseline B/s':>14}  {'optimized B/s':>15}  {'speedup':>9}")
    print("-" * 60)
    for x, b, o in zip(xs, ys_base, ys_opt):
        speedup = (o / b) if b > 0 else 0.0
        print(f"{x:>12d}  {b:>14d}  {o:>15d}  {speedup:>8.2f}x")
    print(f"{'='*60}\n")


def plot(xs, ys_base, ys_opt, header: str, output: Path | None, show: bool):
    try:
        import matplotlib
        if not show:
            matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        sys.exit("matplotlib/numpy not installed. Run: pip install matplotlib numpy")

    xlabel = detect_xlabel(header, default="parameter")
    yb_kb = [v / 1024.0 for v in ys_base]
    yo_kb = [v / 1024.0 for v in ys_opt]
    speedups = [(o / b) if b > 0 else 0.0 for b, o in zip(ys_base, ys_opt)]

    fig, axes = plt.subplots(1, 3, figsize=(16, 5))

    # 1) Grouped bar chart: baseline vs optimized
    ax = axes[0]
    positions = np.arange(len(xs))
    width = 0.38
    ax.bar(positions - width/2, yb_kb, width, label="Baseline",  color="#94a3b8")
    ax.bar(positions + width/2, yo_kb, width, label="Optimized", color="#3b82f6")
    ax.set_xticks(positions)
    ax.set_xticklabels([str(x) for x in xs], rotation=30)
    ax.set_xlabel(xlabel)
    ax.set_ylabel("Throughput (KB/s)")
    ax.set_title("Baseline vs Optimized")
    ax.legend()
    for i, (b, o) in enumerate(zip(yb_kb, yo_kb)):
        ax.text(i - width/2, b, f"{b:.0f}", ha="center", va="bottom", fontsize=7)
        ax.text(i + width/2, o, f"{o:.0f}", ha="center", va="bottom", fontsize=7)

    # 2) Line chart with log2 x-axis
    ax = axes[1]
    ax.plot(xs, yb_kb, marker="o", color="#94a3b8", label="Baseline")
    ax.plot(xs, yo_kb, marker="o", color="#3b82f6", label="Optimized")
    ax.set_xscale("log", base=2)
    ax.set_xlabel(f"{xlabel} (log2)")
    ax.set_ylabel("Throughput (KB/s)")
    ax.set_title("Throughput trend")
    ax.grid(True, which="both", linestyle=":", alpha=0.5)
    ax.legend()

    # 3) Speedup chart
    ax = axes[2]
    ax.bar(positions, speedups, color="#22c55e")
    ax.axhline(1.0, color="#ef4444", linestyle="--", linewidth=1, label="No change")
    ax.set_xticks(positions)
    ax.set_xticklabels([str(x) for x in xs], rotation=30)
    ax.set_xlabel(xlabel)
    ax.set_ylabel("Speedup (×)")
    ax.set_title("Optimized / Baseline")
    ax.legend()
    for i, s in enumerate(speedups):
        ax.text(i, s, f"{s:.2f}x", ha="center", va="bottom", fontsize=8)

    title = "xv6 pipe IPC throughput: baseline vs optimized"
    if header:
        title = f"{title}\n({header})"
    fig.suptitle(title)
    fig.tight_layout()

    if output:
        fig.savefig(output, dpi=150)
        print(f"Saved chart to {output}")
    if show or not output:
        plt.show()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Compare two xv6 pipe IPC throughput results."
    )
    parser.add_argument("baseline",  help="Throughput file produced before optimization")
    parser.add_argument("optimized", help="Throughput file produced after  optimization")
    parser.add_argument(
        "-o", "--output",
        default="compare.png",
        help="Output PNG path (default: compare.png)",
    )
    parser.add_argument(
        "--show",
        action="store_true",
        help="Open an interactive window in addition to saving.",
    )
    args = parser.parse_args()

    base_path = Path(args.baseline)
    opt_path = Path(args.optimized)
    for p in (base_path, opt_path):
        if not p.exists():
            sys.exit(f"Input file not found: {p}")

    base_header, base_points = parse_data(base_path)
    opt_header,  opt_points  = parse_data(opt_path)
    if not base_points or not opt_points:
        sys.exit("No data points parsed from one of the inputs")

    header = base_header or opt_header
    xlabel = detect_xlabel(header, default="parameter")

    xs, ys_base, ys_opt = align_points(base_points, opt_points)
    if not xs:
        sys.exit("Baseline and optimized files have no overlapping x-axis values")

    print(f"Loaded baseline:  {len(base_points)} points from {base_path}")
    print(f"Loaded optimized: {len(opt_points)} points from {opt_path}")
    if header:
        print(f"Header: {header}")
    print_table(xs, ys_base, ys_opt, xlabel)

    out = Path(args.output) if args.output else None
    plot(xs, ys_base, ys_opt, header, out, args.show)


if __name__ == "__main__":
    main()
