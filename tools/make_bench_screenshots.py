#!/usr/bin/env python3
"""
tools/make_bench_screenshots.py
--------------------------------
Tạo terminal-style PNG screenshots từ kết quả pipebench (ipctp_vX.txt)
cho từng cặp phiên bản, rồi tổng hợp thành ảnh so sánh.

Output files (saved to baocaohdh/):
  bench_v1.png          -- v1 standalone
  bench_v1_v2.png       -- so sánh v1 vs v2
  bench_v2_v3.png       -- so sánh v2 vs v3
  bench_v3_v4.png       -- so sánh v3 vs v4
  bench_v4_v5.png       -- so sánh v4 vs v5
  bench_v5_v6.png       -- so sánh v5 vs v6
  bench_v6_v7.png       -- so sánh v6 vs v7
  bench_all.png         -- bảng tổng hợp v1..v8
"""

from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
RESULTS_DIR = ROOT / "results"
OUT_DIR = ROOT / "baocaohdh"

FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
FONT_BOLD_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
FONT_SIZE = 15

BG_COLOR    = (18, 18, 18)
FG_COLOR    = (220, 220, 220)
GREEN_COLOR = (80, 220, 80)
CYAN_COLOR  = (80, 200, 255)
YELLOW_COLOR= (255, 220, 60)
RED_COLOR   = (255, 80, 80)
DIM_COLOR   = (130, 130, 130)
TITLE_COLOR = (255, 200, 60)
HEADER_BG   = (30, 30, 30)
BORDER_COLOR= (60, 120, 60)

VERSION_NAMES = {
    1: "v1 baseline",
    2: "v2 bulk copy",
    3: "v3 ring buffers",
    4: "v4 multi-page",
    5: "v5 lazy wakeup",
    6: "v6 priority boost",
    7: "v7 cache-line align",
    8: "v8 newpipe",
}

CHUNK_VALUES = [64, 128, 256, 512, 1024, 2048, 4096]
PAD = 20
LINE_H = 22


def load_font(bold=False):
    path = FONT_BOLD_PATH if bold else FONT_PATH
    try:
        return ImageFont.truetype(path, FONT_SIZE)
    except Exception:
        return ImageFont.load_default()


def load_data(version: int) -> dict:
    path = RESULTS_DIR / f"ipctp_v{version}.txt"
    data = {}
    if not path.exists():
        return data
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) == 2:
            try:
                data[int(parts[0])] = int(parts[1])
            except ValueError:
                pass
    return data


def fmt_bps(bps: int) -> str:
    if bps >= 1_000_000:
        return f"{bps/1_000_000:.2f} MB/s"
    elif bps >= 1_000:
        return f"{bps/1_000:.1f} KB/s"
    return f"{bps} B/s"


def text_width(draw, text, font):
    bbox = draw.textbbox((0, 0), text, font=font)
    return bbox[2] - bbox[0]


def make_single_version_image(version: int) -> Path:
    """Tạo PNG cho 1 version: giống terminal chạy pipebench."""
    data = load_data(version)
    font = load_font()
    font_bold = load_font(bold=True)

    # Build lines
    lines = []
    lines.append(("cyan",  f"$ pipebench chunk 4194304"))
    lines.append(("dim",   f""))
    lines.append(("title", f"==== IPC throughput sweep: vary chunk_bytes ===="))
    lines.append(("fg",    f"  total_bytes = 4194304"))
    lines.append(("fg",    f"  chunk_bytes  |  B/s"))
    lines.append(("dim",   f"  -------------|-------------------"))
    for c in CHUNK_VALUES:
        bps = data.get(c, 0)
        lines.append(("green", f"  {c:<5}        |  {bps:>12}  ({fmt_bps(bps)})"))
    lines.append(("dim",   f""))
    lines.append(("cyan",  f"Saved results to ipctp.txt"))
    lines.append(("dim",   f"$ "))

    # Size
    dummy_img = Image.new("RGB", (1, 1))
    dummy_draw = ImageDraw.Draw(dummy_img)
    max_w = max(text_width(dummy_draw, l[1], font) for l in lines) + PAD * 2
    h = LINE_H * len(lines) + PAD * 2 + 40  # 40 for title bar

    img = Image.new("RGB", (max_w, h), BG_COLOR)
    draw = ImageDraw.Draw(img)

    # Title bar
    vname = VERSION_NAMES.get(version, f"v{version}")
    draw.rectangle([(0, 0), (max_w, 32)], fill=HEADER_BG)
    title_text = f"  xv6-riscv: pipebench [{vname}]"
    draw.text((PAD, 8), title_text, font=font_bold, fill=TITLE_COLOR)

    # Content
    color_map = {
        "fg": FG_COLOR, "cyan": CYAN_COLOR, "green": GREEN_COLOR,
        "dim": DIM_COLOR, "title": YELLOW_COLOR, "red": RED_COLOR,
    }
    y = 40 + PAD // 2
    for (color_key, text) in lines:
        c = color_map.get(color_key, FG_COLOR)
        draw.text((PAD, y), text, font=font, fill=c)
        y += LINE_H

    out_path = OUT_DIR / f"bench_v{version}.png"
    img.save(str(out_path))
    print(f"  Saved: {out_path.name}")
    return out_path


def make_comparison_image(v1: int, v2: int) -> Path:
    """Tạo PNG so sánh 2 version side-by-side."""
    d1 = load_data(v1)
    d2 = load_data(v2)
    font = load_font()
    font_bold = load_font(bold=True)

    n1 = VERSION_NAMES.get(v1, f"v{v1}")
    n2 = VERSION_NAMES.get(v2, f"v{v2}")

    # Build lines for left and right panels
    def make_lines(vn, data):
        ls = []
        ls.append(("title", f"[{vn}]"))
        ls.append(("dim",   f"chunk  |  B/s              | rate"))
        ls.append(("dim",   f"-------|-------------------|--------"))
        for c in CHUNK_VALUES:
            bps = data.get(c, 0)
            ls.append(("green", f"{c:<5}  |  {bps:>12}  | {fmt_bps(bps)}"))
        return ls

    lines1 = make_lines(n1, d1)
    lines2 = make_lines(n2, d2)

    dummy_img = Image.new("RGB", (1, 1))
    dummy_draw = ImageDraw.Draw(dummy_img)

    col_w = max(
        max(text_width(dummy_draw, l[1], font) for l in lines1),
        max(text_width(dummy_draw, l[1], font) for l in lines2),
    ) + PAD * 2

    n_lines = max(len(lines1), len(lines2))
    h = LINE_H * (n_lines + 4) + PAD * 2 + 40  # extra lines for header

    total_w = col_w * 2 + 6  # 6px divider

    img = Image.new("RGB", (total_w, h), BG_COLOR)
    draw = ImageDraw.Draw(img)

    # Title bar
    draw.rectangle([(0, 0), (total_w, 32)], fill=HEADER_BG)
    title = f"  xv6-riscv: pipebench throughput comparison"
    draw.text((PAD, 8), title, font=font_bold, fill=TITLE_COLOR)

    # Divider line
    draw.line([(col_w + 3, 32), (col_w + 3, h)], fill=BORDER_COLOR, width=2)

    color_map = {
        "fg": FG_COLOR, "cyan": CYAN_COLOR, "green": GREEN_COLOR,
        "dim": DIM_COLOR, "title": YELLOW_COLOR,
    }

    # Command echo
    y = 40 + PAD // 2
    draw.text((PAD, y), f"$ pipebench chunk 4194304", font=font, fill=CYAN_COLOR)
    draw.text((col_w + 6 + PAD, y), f"$ pipebench chunk 4194304", font=font, fill=CYAN_COLOR)
    y += LINE_H * 2

    for i in range(max(len(lines1), len(lines2))):
        if i < len(lines1):
            ck, txt = lines1[i]
            draw.text((PAD, y), txt, font=font, fill=color_map.get(ck, FG_COLOR))
        if i < len(lines2):
            ck2, txt2 = lines2[i]
            # Highlight speedup for data rows
            col = color_map.get(ck2, FG_COLOR)
            if ck2 == "green":
                # Check speedup
                try:
                    c = CHUNK_VALUES[i - 3]  # offset: 3 header lines
                    bps1 = d1.get(c, 0)
                    bps2 = d2.get(c, 0)
                    if bps1 > 0 and bps2 / bps1 >= 2.0:
                        col = (255, 255, 80)  # bright yellow for big speedup
                    elif bps1 > 0 and bps2 / bps1 >= 1.5:
                        col = (80, 255, 120)  # bright green for moderate
                except (IndexError, ZeroDivisionError):
                    pass
            draw.text((col_w + 6 + PAD, y), txt2, font=font, fill=col)
        y += LINE_H

    # Speedup summary row
    y += LINE_H // 2
    draw.line([(PAD, y), (total_w - PAD, y)], fill=DIM_COLOR, width=1)
    y += LINE_H // 2

    speedups = []
    for c in CHUNK_VALUES:
        b1 = d1.get(c, 0)
        b2 = d2.get(c, 0)
        if b1 > 0:
            speedups.append(b2 / b1)
    if speedups:
        avg_sp = sum(speedups) / len(speedups)
        max_sp = max(speedups)
        summary = f"  Speedup: avg {avg_sp:.1f}x   max {max_sp:.1f}x   ({n1} -> {n2})"
        draw.text((PAD, y), summary, font=font_bold, fill=YELLOW_COLOR)

    out_path = OUT_DIR / f"bench_v{v1}_v{v2}.png"
    img.save(str(out_path))
    print(f"  Saved: {out_path.name}")
    return out_path


def make_all_versions_table() -> Path:
    """Tạo bảng tổng hợp v1..v8 dưới dạng terminal."""
    all_data = {v: load_data(v) for v in range(1, 9)}
    font = load_font()
    font_bold = load_font(bold=True)

    versions = [v for v in range(1, 9) if all_data[v]]
    vnames = [VERSION_NAMES.get(v, f"v{v}") for v in versions]

    dummy_img = Image.new("RGB", (1, 1))
    dummy_draw = ImageDraw.Draw(dummy_img)

    # Column widths
    chunk_col_w = text_width(dummy_draw, "chunk_bytes", font) + 10
    val_col_w = text_width(dummy_draw, "  20.97 MB/s  ", font) + 6

    total_w = chunk_col_w + val_col_w * len(versions) + PAD * 2
    n_data_lines = len(CHUNK_VALUES)
    h = LINE_H * (n_data_lines + len(versions) + 8) + PAD * 2 + 40

    img = Image.new("RGB", (total_w, h), BG_COLOR)
    draw = ImageDraw.Draw(img)

    # Title bar
    draw.rectangle([(0, 0), (total_w, 32)], fill=HEADER_BG)
    draw.text((PAD, 8),
              "  xv6-riscv: pipebench throughput -- v1 through v8",
              font=font_bold, fill=TITLE_COLOR)

    y = 40 + PAD // 2

    # Version name header
    draw.text((PAD, y), "chunk_bytes", font=font_bold, fill=CYAN_COLOR)
    for i, v in enumerate(versions):
        x = PAD + chunk_col_w + i * val_col_w
        draw.text((x, y), f"  v{v}", font=font_bold, fill=CYAN_COLOR)
    y += LINE_H

    # Separator
    draw.line([(PAD, y), (total_w - PAD, y)], fill=DIM_COLOR, width=1)
    y += LINE_H // 2

    # Data rows
    for c in CHUNK_VALUES:
        draw.text((PAD, y), f"{c}", font=font, fill=FG_COLOR)
        for i, v in enumerate(versions):
            bps = all_data[v].get(c, 0)
            x = PAD + chunk_col_w + i * val_col_w
            # Color by magnitude
            if bps >= 10_000_000:
                col = (255, 220, 60)
            elif bps >= 5_000_000:
                col = (80, 255, 120)
            elif bps >= 1_000_000:
                col = (80, 200, 255)
            else:
                col = FG_COLOR
            draw.text((x, y), f"  {fmt_bps(bps)}", font=font, fill=col)
        y += LINE_H

    # Separator
    y += LINE_H // 4
    draw.line([(PAD, y), (total_w - PAD, y)], fill=DIM_COLOR, width=1)
    y += LINE_H

    # Speedup vs v1 row
    draw.text((PAD, y), "speedup vs v1:", font=font_bold, fill=YELLOW_COLOR)
    y += LINE_H
    base = all_data.get(1, {})
    for v in versions:
        if v == 1:
            continue
        d = all_data[v]
        speedups = [d[c] / base[c] for c in CHUNK_VALUES if c in base and base[c] > 0 and c in d]
        if speedups:
            avg_sp = sum(speedups) / len(speedups)
            max_sp = max(speedups)
            vn = VERSION_NAMES.get(v, f"v{v}")
            line = f"  v{v} ({vn:<22}):  avg {avg_sp:5.1f}x   max {max_sp:5.1f}x"
            draw.text((PAD, y), line, font=font, fill=GREEN_COLOR)
            y += LINE_H

    out_path = OUT_DIR / "bench_all.png"
    img.save(str(out_path))
    print(f"  Saved: {out_path.name}")
    return out_path


def main():
    print("Generating benchmark screenshots ...")
    print()

    # Single version
    print("[1/10] v1 standalone ...")
    make_single_version_image(1)

    # Comparison pairs as in report
    pairs = [(1, 2), (2, 3), (3, 4), (4, 5), (5, 6), (6, 7)]
    for i, (a, b) in enumerate(pairs, 2):
        print(f"[{i}/10] v{a} vs v{b} comparison ...")
        make_comparison_image(a, b)

    # All versions table
    print("[9/10] All versions summary table ...")
    make_all_versions_table()

    print()
    print("Done. Files saved to baocaohdh/:")
    for f in sorted(OUT_DIR.glob("bench_*.png")):
        print(f"  {f.name}")


if __name__ == "__main__":
    main()
