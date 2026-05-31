#!/usr/bin/env python3
"""
Vẽ biểu đồ phân tách overhead của pipe IPC trong xv6.

Đọc file overhead.txt (output của chương trình pipeoverhead trong xv6),
vẽ 4 biểu đồ:
  1. Stacked bar: phân tách thời gian (syscall OH vs sleep/wakeup OH)
  2. Line chart:  số syscalls & số sleeps theo chunk_bytes
  3. Pie chart:   tỉ lệ overhead ở chunk nhỏ nhất vs lớn nhất
  4. Bar chart:   throughput theo chunk_bytes (so sánh với pipebench)

File format overhead.txt:
    # PIPE OVERHEAD ANALYSIS
    # total_bytes=4194304 PIPESIZE=512 us_per_syscall=10
    # chunk total_us N_syscalls syscall_oh_us N_sleeps other_oh_us syscall_oh_pct throughput_bps
    64 8500000 131072 1310720 16384 7189280 15 493525
    128 5200000 65536 655360 16384 4544640 12 807692
    ...

Workflow:
    1. Trong xv6:  pipeoverhead           # chạy benchmark, tạo overhead.txt
                   cat overhead.txt       # in ra console
    2. Copy nội dung vào file trên host, vd: overhead.txt
    3. Trên host:  python3 tools/plot_overhead.py overhead.txt

Usage:
    python3 tools/plot_overhead.py [input.txt] [-o output.png] [--show]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def parse_data(path: Path):
    """Đọc file overhead.txt, trả về metadata và danh sách records."""
    meta: dict[str, str] = {}
    records: list[dict[str, int]] = []

    with path.open("r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            if line.startswith("#"):
                # Parse metadata từ header comments
                content = line.lstrip("#").strip()
                for token in content.split():
                    if "=" in token:
                        k, v = token.split("=", 1)
                        meta[k] = v
                continue

            parts = line.split()
            if len(parts) < 8:
                continue
            try:
                rec = {
                    "chunk":         int(parts[0]),
                    "total_us":      int(parts[1]),
                    "n_syscalls":    int(parts[2]),
                    "syscall_oh_us": int(parts[3]),
                    "n_sleeps":      int(parts[4]),
                    "other_oh_us":   int(parts[5]),
                    "syscall_pct":   int(parts[6]),
                    "throughput":    int(parts[7]),
                }
            except ValueError:
                continue
            records.append(rec)

    return meta, records


def plot(records, meta, output: Path | None, show: bool):
    try:
        import matplotlib
        if not show:
            matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import matplotlib.ticker as mticker
    except ImportError:
        sys.exit("matplotlib not installed. Run: pip install matplotlib")

    # --------------- Chuẩn bị dữ liệu ---------------
    chunks     = [r["chunk"] for r in records]
    total_us   = [r["total_us"] for r in records]
    n_syscalls = [r["n_syscalls"] for r in records]
    syscall_oh = [r["syscall_oh_us"] for r in records]
    n_sleeps   = [r["n_sleeps"] for r in records]
    other_oh   = [r["other_oh_us"] for r in records]
    throughput = [r["throughput"] for r in records]
    labels     = [str(c) for c in chunks]

    # Quy đổi sang ms để dễ đọc
    total_ms     = [v / 1000.0 for v in total_us]
    syscall_ms   = [v / 1000.0 for v in syscall_oh]
    other_ms     = [v / 1000.0 for v in other_oh]
    throughput_kb = [v / 1024.0 for v in throughput]

    total_bytes = meta.get("total_bytes", "?")
    pipesize    = meta.get("PIPESIZE", "512")

    # --------------- Tạo figure 2×2 ---------------
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.patch.set_facecolor("#f8f9fa")

    colors = {
        "syscall":    "#e74c3c",   # đỏ - syscall overhead
        "sleepwake":  "#f39c12",   # cam - sleep/wakeup overhead
        "throughput": "#3498db",   # xanh dương
        "nsyscall":   "#e74c3c",   # đỏ
        "nsleep":     "#f39c12",   # cam
    }

    # ===== BIỂU ĐỒ 1: Stacked Area - Phân tách thời gian =====
    ax = axes[0][0]
    ax.fill_between(chunks, 0, syscall_ms,
                    label="Syscall overhead", color=colors["syscall"],
                    alpha=0.8)
    ax.fill_between(chunks, syscall_ms, total_ms,
                    label="Sleep/wakeup + lock + copy overhead",
                    color=colors["sleepwake"], alpha=0.8)
    ax.plot(chunks, total_ms, color="#2c3e50", linewidth=1.5,
            label="Tổng thời gian")

    ax.set_xlabel("chunk_bytes (B)")
    ax.set_ylabel("Thời gian (ms)")
    ax.set_title("① Phân tách thời gian overhead", fontweight="bold")
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(axis="y", linestyle=":", alpha=0.4)
    ax.set_xlim(chunks[0], chunks[-1])

    # ===== BIỂU ĐỒ 2: Line - Số sự kiện overhead =====
    ax = axes[0][1]
    ax.plot(chunks, n_syscalls, marker="o", color=colors["nsyscall"],
            linewidth=2, markersize=4, label="Số syscalls (write+read)")
    ax.plot(chunks, n_sleeps, marker="s", color=colors["nsleep"],
            linewidth=2, markersize=4, linestyle="--",
            label="Số sleep/wakeup (≈ 2×total/PIPESIZE)")

    ax.set_yscale("log")
    ax.set_xlabel("chunk_bytes (B)")
    ax.set_ylabel("Số lần (log)")
    ax.set_title("② Số sự kiện overhead theo chunk_bytes", fontweight="bold")
    ax.legend(fontsize=8)
    ax.grid(True, which="both", linestyle=":", alpha=0.4)
    ax.set_xlim(chunks[0], chunks[-1])

    # Annotate N_sleeps là hằng số
    if n_sleeps:
        sleep_val = n_sleeps[0]
        ax.axhline(y=sleep_val, color=colors["nsleep"],
                   linestyle=":", alpha=0.6)
        mid_x = chunks[len(chunks) // 2]
        ax.annotate(
            f"N_sleep = {sleep_val}\n(HẰNG SỐ - không đổi!)",
            xy=(mid_x, sleep_val),
            xytext=(mid_x, sleep_val * 4),
            fontsize=8, fontweight="bold", color=colors["nsleep"],
            arrowprops=dict(arrowstyle="->", color=colors["nsleep"]),
            bbox=dict(boxstyle="round,pad=0.3", facecolor="#fff3cd",
                      edgecolor=colors["nsleep"]),
            ha="center",
        )

    # ===== BIỂU ĐỒ 3: So sánh Pie chart (chunk nhỏ nhất vs lớn nhất) =====
    ax = axes[1][0]
    if len(records) >= 2:
        first = records[0]   # chunk nhỏ nhất
        last  = records[-1]  # chunk lớn nhất

        pie_data = [
            [first["syscall_oh_us"], first["other_oh_us"]],
            [last["syscall_oh_us"],  last["other_oh_us"]],
        ]
        pie_labels = ["Syscall OH", "Sleep/wake\n+ khác"]
        pie_colors = [colors["syscall"], colors["sleepwake"]]
        titles = [f"chunk = {first['chunk']}B", f"chunk = {last['chunk']}B"]

        for idx, (data, title) in enumerate(zip(pie_data, titles)):
            ax_sub = fig.add_axes([
                0.08 + idx * 0.22,   # x
                0.08,                # y
                0.20,                # width
                0.38,                # height
            ])
            wedges, texts, autotexts = ax_sub.pie(
                data, labels=pie_labels, autopct="%1.1f%%",
                colors=pie_colors, startangle=90,
                textprops={"fontsize": 8},
            )
            ax_sub.set_title(title, fontsize=9, fontweight="bold")

        # Ẩn ax gốc, dùng sub-axes thay thế
        ax.set_visible(False)

    # ===== BIỂU ĐỒ 4: Throughput line chart =====
    ax = axes[1][1]
    ax.plot(chunks, throughput_kb, marker="o", color=colors["throughput"],
            linewidth=2, markersize=4)
    ax.fill_between(chunks, 0, throughput_kb,
                    color=colors["throughput"], alpha=0.15)

    ax.set_xlabel("chunk_bytes (B)")
    ax.set_ylabel("Throughput (KB/s)")
    ax.set_title("④ Throughput theo chunk_bytes", fontweight="bold")
    ax.grid(True, linestyle=":", alpha=0.4)
    ax.set_xlim(chunks[0], chunks[-1])

    # Annotate điểm đầu và cuối
    ax.annotate(f"{throughput_kb[0]:.0f}",
                xy=(chunks[0], throughput_kb[0]),
                xytext=(chunks[0] + 200, throughput_kb[0] * 0.85),
                fontsize=8, fontweight="bold",
                arrowprops=dict(arrowstyle="->", color=colors["throughput"]))
    ax.annotate(f"{throughput_kb[-1]:.0f}",
                xy=(chunks[-1], throughput_kb[-1]),
                xytext=(chunks[-1] - 600, throughput_kb[-1] * 1.1),
                fontsize=8, fontweight="bold",
                arrowprops=dict(arrowstyle="->", color=colors["throughput"]))

    # --------------- Tiêu đề chung ---------------
    suptitle = "Phân tích Overhead của Pipe IPC trong xv6"
    subtitle = f"total_bytes = {total_bytes}  |  PIPESIZE = {pipesize}"
    fig.suptitle(f"{suptitle}\n{subtitle}",
                 fontsize=14, fontweight="bold", y=0.98)
    fig.subplots_adjust(left=0.08, right=0.96, top=0.88, bottom=0.08,
                        wspace=0.3, hspace=0.35)

    # --------------- Lưu / hiển thị ---------------
    if output:
        fig.savefig(output, dpi=150, bbox_inches="tight",
                    facecolor=fig.get_facecolor())
        print(f"Saved chart to {output}")
    if show or not output:
        plt.show()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Vẽ biểu đồ phân tách overhead của pipe IPC trong xv6."
    )
    parser.add_argument(
        "input",
        nargs="?",
        default="overhead.txt",
        help="Path to overhead data file (default: overhead.txt)",
    )
    parser.add_argument(
        "-o", "--output",
        default="overhead.png",
        help="Output PNG path (default: overhead.png)",
    )
    parser.add_argument(
        "--show",
        action="store_true",
        help="Open an interactive window in addition to saving.",
    )
    args = parser.parse_args()

    path = Path(args.input)
    if not path.exists():
        sys.exit(f"Input file not found: {path}")

    meta, records = parse_data(path)
    if not records:
        sys.exit(f"No data records parsed from {path}")

    print(f"Loaded {len(records)} data points from {path}")
    if meta:
        print(f"Metadata: {meta}")

    print()
    print(f"  {'chunk':>7s}  {'total_us':>10s}  {'N_sys':>7s}  "
          f"{'sys_oh_us':>10s}  {'N_sleep':>7s}  "
          f"{'other_oh':>10s}  {'sys%':>5s}  {'KB/s':>8s}")
    print("  " + "-" * 75)
    for r in records:
        print(f"  {r['chunk']:>7d}  {r['total_us']:>10d}  "
              f"{r['n_syscalls']:>7d}  {r['syscall_oh_us']:>10d}  "
              f"{r['n_sleeps']:>7d}  {r['other_oh_us']:>10d}  "
              f"{r['syscall_pct']:>4d}%  {r['throughput']/1024:>8.1f}")

    out = Path(args.output) if args.output else None
    plot(records, meta, out, args.show)


if __name__ == "__main__":
    main()
