#!/usr/bin/env python3
# ============================================================================
#  tools/plot_compare.py
# ----------------------------------------------------------------------------
#  Read results/compare.csv produced by run_compare.py and draw:
#
#    1) Multi-line throughput chart (1 line per PIPE_VERSION,
#       x = chunk_bytes log2, y = KB/s)
#    2) Speedup chart vs v1 baseline (bar groups per chunk_bytes)
#    3) Stacked-style summary at largest chunk_bytes
#
#  USAGE:
#      python3 tools/plot_compare.py [results/compare.csv]
#                                    [-o results/compare_throughput.png]
#                                    [--show]
# ============================================================================

import argparse
import csv
import sys
from collections import defaultdict
from pathlib import Path


VERSION_LABELS = {
    1: "v1: baseline (byte loop, 512B)",
    2: "v2: bulk copy",
    3: "v3: ring of buffers (2KB)",
    4: "v4: multi-page (4KB)",
    5: "v5: lazy wakeup",
    6: "v6: priority boost",
    7: "v7: cache-line aligned",
    8: "v8: newpipe (8KB, 2×page, all opts)",
}

# Distinct, color-blind friendly palette (Tol/Bright)
VERSION_COLORS = {
    1: "#94a3b8",  # slate
    2: "#3b82f6",  # blue
    3: "#10b981",  # emerald
    4: "#f59e0b",  # amber
    5: "#ef4444",  # red
    6: "#8b5cf6",  # violet
    7: "#0f172a",  # near-black
    8: "#f97316",  # orange
}


def load_csv(path: Path):
    """Returns: {version_int: [(chunk, bps), ...]} sorted by chunk."""
    by_ver: dict[int, list[tuple[int, int]]] = defaultdict(list)
    with path.open() as f:
        reader = csv.DictReader(f)
        for row in reader:
            v = int(row["version"])
            chunk = int(row["chunk_bytes"])
            bps = int(row["throughput_bps"])
            by_ver[v].append((chunk, bps))
    for v in by_ver:
        by_ver[v].sort()
    return dict(by_ver)


def plot(results, output: Path | None, show: bool):
    try:
        import matplotlib
        if not show:
            matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        sys.exit("matplotlib/numpy not installed: pip install matplotlib numpy")

    versions = sorted(results.keys())
    all_chunks = sorted({c for rows in results.values() for c, _ in rows})

    fig, axes = plt.subplots(1, 2, figsize=(15, 6))

    # -----------------------------------------------------------------
    # Plot 1: throughput trends, one line per version
    # -----------------------------------------------------------------
    ax = axes[0]
    for v in versions:
        xs = [c for c, _ in results[v]]
        ys = [b / 1024.0 for _, b in results[v]]   # KB/s
        ax.plot(xs, ys,
                marker="o", linewidth=2,
                label=VERSION_LABELS.get(v, f"v{v}"),
                color=VERSION_COLORS.get(v))
    ax.set_xscale("log", base=2)
    ax.set_xlabel("chunk_bytes (log2)")
    ax.set_ylabel("Throughput (KB/s)")
    ax.set_title("Pipe IPC throughput per mechanism")
    ax.grid(True, which="both", linestyle=":", alpha=0.5)
    ax.legend(loc="upper left", fontsize=9)

    # -----------------------------------------------------------------
    # Plot 2: speedup vs v1 baseline (grouped bar)
    # -----------------------------------------------------------------
    ax = axes[1]
    if 1 in results:
        baseline = dict(results[1])
        bar_w = 0.80 / max(1, len(versions) - 1)
        positions = np.arange(len(all_chunks))
        non_v1 = [v for v in versions if v != 1]
        for i, v in enumerate(non_v1):
            d = dict(results[v])
            speedups = [
                (d.get(c, 0) / baseline[c]) if baseline.get(c, 0) else 0
                for c in all_chunks
            ]
            offset = (i - (len(non_v1) - 1) / 2) * bar_w
            ax.bar(positions + offset, speedups, bar_w,
                   label=f"v{v}",
                   color=VERSION_COLORS.get(v))
        ax.axhline(1.0, color="#94a3b8", linestyle="--",
                   linewidth=1, label="v1 baseline")
        ax.set_xticks(positions)
        ax.set_xticklabels([str(c) for c in all_chunks], rotation=30)
        ax.set_xlabel("chunk_bytes")
        ax.set_ylabel("Speedup (×)  vs v1")
        ax.set_title("Speedup of each mechanism vs v1 baseline")
        ax.grid(True, axis="y", linestyle=":", alpha=0.5)
        ax.legend(loc="upper left", fontsize=9, ncol=2)
    else:
        ax.text(0.5, 0.5, "no v1 baseline -> cannot compute speedup",
                transform=ax.transAxes, ha="center")
        ax.set_axis_off()

    fig.suptitle("xv6 pipe IPC: comparison of optimization mechanisms",
                 fontsize=13)
    fig.tight_layout()

    if output:
        fig.savefig(output, dpi=150)
        print(f"Saved chart to {output}")
    if show or not output:
        plt.show()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv",
                    nargs="?",
                    default="results/compare.csv",
                    help="path to compare.csv (default: results/compare.csv)")
    ap.add_argument("-o", "--output",
                    default="results/compare_throughput.png",
                    help="output PNG path")
    ap.add_argument("--show", action="store_true",
                    help="open interactive window in addition to saving")
    args = ap.parse_args()

    csv_path = Path(args.csv)
    if not csv_path.exists():
        sys.exit(f"CSV not found: {csv_path}")
    results = load_csv(csv_path)
    if not results:
        sys.exit(f"no rows parsed from {csv_path}")

    print(f"Loaded {sum(len(r) for r in results.values())} rows "
          f"across {len(results)} versions from {csv_path}")
    plot(results, Path(args.output) if args.output else None, args.show)


if __name__ == "__main__":
    main()
