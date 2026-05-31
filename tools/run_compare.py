#!/usr/bin/env python3
# ============================================================================
#  tools/run_compare.py
# ----------------------------------------------------------------------------
#  Host orchestrator that compares xv6 pipe IPC throughput across all
#  PIPE_VERSION mechanisms (1..8).
#
#  WORKFLOW (per version v):
#      1. make clean && make PIPE_VERSION=v fs.img kernel/kernel
#      2. Boot QEMU, wait for shell prompt ("$ ")
#      3. Send: pipebench chunk <total_bytes>
#      4. Wait for "Saved results" line, then `cat ipctp.txt`
#      5. Parse stdout into (chunk_bytes -> throughput_bps)
#      6. Quit QEMU (Ctrl-A x)
#      7. Append rows to results/compare.csv
#
#  After all 8 versions, optionally invoke plot_compare.py automatically.
#  When --versions is a subset, existing compare.csv data for other versions
#  is preserved and merged into the output CSV automatically.
#
#  USAGE:
#      python3 tools/run_compare.py                  # full sweep (4MB)
#      python3 tools/run_compare.py --total 1048576  # 1MB sweep
#      python3 tools/run_compare.py --versions 1,4,8 # subset (merges rest)
#      python3 tools/run_compare.py --plot           # also draw chart
# ============================================================================

import argparse
import csv as _csv
import os
import re
import select
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RESULTS_DIR = ROOT / "results"
RESULTS_DIR.mkdir(exist_ok=True)

ALL_VERSIONS = [1, 2, 3, 4, 5, 6, 7, 8]
VERSION_NAMES = {
    1: "v1_baseline",
    2: "v2_bulk_copy",
    3: "v3_ring_buffers",
    4: "v4_multi_page",
    5: "v5_lazy_wakeup",
    6: "v6_priority_boost",
    7: "v7_cache_align",
    8: "v8_newpipe",
}

PROMPT_RE = re.compile(rb"\$\s*$")
SAVED_RE  = re.compile(rb"Saved results")
DATA_RE   = re.compile(r"^\s*(\d+)\s+(\d+)\s*$")


# ----------------------------------------------------------------------------
#  Build helpers
# ----------------------------------------------------------------------------
def run_make(version: int) -> None:
    print(f"[v{version}] make clean ...", flush=True)
    subprocess.run(["make", "clean"], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # Build BOTH the kernel ELF and the filesystem image (fs.img alone
    # doesn't pull in kernel/kernel, but QEMU needs it).
    print(f"[v{version}] make PIPE_VERSION={version} kernel + fs.img ...",
          flush=True)
    res = subprocess.run(
        ["make", f"PIPE_VERSION={version}", "kernel/kernel", "fs.img"],
        cwd=ROOT, capture_output=True, text=True,
    )
    if res.returncode != 0:
        sys.stderr.write(res.stdout)
        sys.stderr.write(res.stderr)
        raise RuntimeError(f"build of v{version} failed")


# ----------------------------------------------------------------------------
#  QEMU interactive driver — works without pexpect, uses pty + select
# ----------------------------------------------------------------------------
class XV6Qemu:
    """Boots a QEMU instance, communicates over its serial console."""

    def __init__(self, cpus: int = 1, log_path: Path | None = None):
        # Use pty so xv6 sees a real terminal (line discipline matters).
        import pty
        master_fd, slave_fd = pty.openpty()
        self.master_fd = master_fd
        cmd = [
            "qemu-system-riscv64",
            "-machine", "virt", "-bios", "none",
            "-kernel", str(ROOT / "kernel" / "kernel"),
            "-m", "128M", "-smp", str(cpus), "-nographic",
            "-global", "virtio-mmio.force-legacy=false",
            "-drive", f"file={ROOT / 'fs.img'},if=none,format=raw,id=x0",
            "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
        ]
        self.proc = subprocess.Popen(
            cmd,
            stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
            preexec_fn=os.setsid,
            close_fds=True,
        )
        os.close(slave_fd)
        self.buf = b""
        self.log_fd = open(log_path, "wb") if log_path else None

    # -- low-level I/O ------------------------------------------------------
    def _read_some(self, timeout: float) -> bytes:
        try:
            r, _, _ = select.select([self.master_fd], [], [], timeout)
        except OSError:
            return b""
        if not r:
            return b""
        try:
            chunk = os.read(self.master_fd, 4096)
        except OSError:
            return b""
        if self.log_fd:
            self.log_fd.write(chunk)
            self.log_fd.flush()
        return chunk

    def expect(self, pattern, timeout: float = 30.0) -> bytes:
        """Read until pattern matches. Returns everything read so far."""
        if isinstance(pattern, str):
            pattern = re.compile(pattern.encode())
        elif isinstance(pattern, bytes):
            pattern = re.compile(pattern)
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = self._read_some(0.5)
            if chunk:
                self.buf += chunk
            if pattern.search(self.buf):
                m = pattern.search(self.buf)
                consumed, self.buf = self.buf[: m.end()], self.buf[m.end():]
                return consumed
            if self.proc.poll() is not None:
                raise RuntimeError("qemu exited early")
        raise TimeoutError(f"timeout waiting for {pattern.pattern!r}")

    def expect_either(self, patterns, timeout: float = 60.0):
        """Wait for any of the given regex patterns; return (idx, data)."""
        compiled = [re.compile(p if isinstance(p, bytes) else p.encode())
                    for p in patterns]
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = self._read_some(0.5)
            if chunk:
                self.buf += chunk
            for i, pat in enumerate(compiled):
                m = pat.search(self.buf)
                if m:
                    consumed, self.buf = self.buf[:m.end()], self.buf[m.end():]
                    return i, consumed
            if self.proc.poll() is not None:
                raise RuntimeError("qemu exited early")
        raise TimeoutError(f"timeout waiting for any of {patterns}")

    def send(self, s: str) -> None:
        os.write(self.master_fd, s.encode())

    def sendline(self, s: str) -> None:
        self.send(s + "\n")

    def quit(self) -> None:
        # Ctrl-A x is QEMU's escape sequence for terminate.
        try:
            self.send("\x01x")
        except OSError:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            self.proc.wait()
        try:
            os.close(self.master_fd)
        except OSError:
            pass
        if self.log_fd:
            self.log_fd.close()


# ----------------------------------------------------------------------------
#  Run pipebench inside xv6 and collect the (chunk, throughput) table
# ----------------------------------------------------------------------------
def run_pipebench(version: int, total_bytes: int) -> list[tuple[int, int]]:
    log_path = RESULTS_DIR / f"qemu_v{version}.log"
    qemu = XV6Qemu(cpus=1, log_path=log_path)

    print(f"[v{version}] booting QEMU ...", flush=True)
    qemu.expect(rb"init: starting sh", timeout=60)
    # Wait for shell prompt; xv6 emits "$ " when ready.
    qemu.expect(rb"\$ ", timeout=30)

    cmd = f"pipebench chunk {total_bytes}"
    print(f"[v{version}] sending: {cmd}", flush=True)
    qemu.sendline(cmd)
    qemu.expect(rb"Saved results to ipctp\.txt",
                timeout=max(180, total_bytes // 20000))
    qemu.expect(rb"\$ ", timeout=30)

    # Print the file — easier than reading from fs.img.
    qemu.sendline("cat ipctp.txt")
    # Capture between the cat command and the next prompt.
    captured = qemu.expect(rb"\$ ", timeout=30).decode("utf-8", "replace")

    qemu.quit()

    # Parse data lines
    rows = []
    for line in captured.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("$"):
            continue
        if line.startswith("cat "):
            continue
        m = DATA_RE.match(line)
        if m:
            rows.append((int(m.group(1)), int(m.group(2))))
    if not rows:
        raise RuntimeError(f"v{version}: no data parsed (see {log_path})")

    # Save per-version raw dump for reproducibility
    raw = RESULTS_DIR / f"ipctp_v{version}.txt"
    with raw.open("w") as f:
        f.write(f"# version=v{version} total_bytes={total_bytes}\n")
        for c, b in rows:
            f.write(f"{c} {b}\n")
    print(f"[v{version}] saved {len(rows)} points -> {raw}", flush=True)
    return rows


# ----------------------------------------------------------------------------
#  Merge newly-run results with any existing CSV rows for skipped versions
# ----------------------------------------------------------------------------
def merge_existing_csv(new_results: dict[int, list[tuple[int, int]]],
                       versions_run: set[int]) -> dict[int, list[tuple[int, int]]]:
    """Return new_results augmented with existing CSV data for versions not re-run."""
    csv_path = RESULTS_DIR / "compare.csv"
    if not csv_path.exists():
        return new_results
    merged = dict(new_results)
    with csv_path.open() as f:
        reader = _csv.DictReader(f)
        for row in reader:
            v = int(row["version"])
            if v not in versions_run:
                merged.setdefault(v, [])
                merged[v].append((int(row["chunk_bytes"]), int(row["throughput_bps"])))
    return merged


# ----------------------------------------------------------------------------
#  CSV writer
# ----------------------------------------------------------------------------
def write_csv(all_results: dict[int, list[tuple[int, int]]],
              total_bytes: int) -> Path:
    csv = RESULTS_DIR / "compare.csv"
    with csv.open("w") as f:
        f.write("version,version_name,chunk_bytes,throughput_bps,total_bytes\n")
        for v in sorted(all_results.keys()):
            for chunk, bps in all_results[v]:
                f.write(f"{v},{VERSION_NAMES[v]},{chunk},{bps},{total_bytes}\n")
    print(f"\nWrote {csv}")
    return csv


# ----------------------------------------------------------------------------
#  ASCII summary table
# ----------------------------------------------------------------------------
def print_table(all_results: dict[int, list[tuple[int, int]]]) -> None:
    if not all_results:
        return
    versions = sorted(all_results.keys())
    chunks = sorted({c for rows in all_results.values() for c, _ in rows})
    print("\n" + "=" * (14 + 12 * len(versions)))
    header = f"{'chunk_bytes':>12}" + "".join(f"{'v'+str(v):>12}" for v in versions)
    print(header)
    print("-" * (14 + 12 * len(versions)))
    base = dict(all_results.get(1, []))
    for c in chunks:
        row = f"{c:>12}"
        for v in versions:
            d = dict(all_results[v])
            bps = d.get(c, 0)
            row += f"{bps:>12}"
        print(row)
    if 1 in all_results and len(versions) > 1:
        print("\nSpeedup vs v1:")
        for v in versions:
            if v == 1:
                continue
            d = dict(all_results[v])
            speedups = [d.get(c, 0) / base[c] if base.get(c, 0) else 0
                        for c in chunks if c in base]
            if speedups:
                avg = sum(speedups) / len(speedups)
                mx = max(speedups)
                print(f"  v{v} {VERSION_NAMES[v]:<22}  avg {avg:5.2f}x   max {mx:5.2f}x")


# ----------------------------------------------------------------------------
#  Main
# ----------------------------------------------------------------------------
def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--total", type=int, default=4 * 1024 * 1024,
                    help="total_bytes for pipebench (default: 4MB)")
    ap.add_argument("--versions", default=",".join(str(v) for v in ALL_VERSIONS),
                    help="comma-separated list of versions to run, e.g. 1,4,7")
    ap.add_argument("--plot", action="store_true",
                    help="also generate compare_throughput.png")
    ap.add_argument("--skip-build", action="store_true",
                    help="skip build (assume kernels are pre-built; debug only)")
    args = ap.parse_args()

    versions = [int(x) for x in args.versions.split(",") if x.strip()]
    for v in versions:
        if v not in ALL_VERSIONS:
            sys.exit(f"unknown version v{v}")

    all_results: dict[int, list[tuple[int, int]]] = {}
    for v in versions:
        if not args.skip_build:
            run_make(v)
        try:
            all_results[v] = run_pipebench(v, args.total)
        except Exception as e:
            print(f"[v{v}] FAILED: {e}", file=sys.stderr)
            continue

    if not all_results:
        sys.exit("no results collected")

    # Preserve existing CSV rows for versions not re-run this session.
    merged = merge_existing_csv(all_results, set(versions))
    write_csv(merged, args.total)
    print_table(merged)

    if args.plot:
        plotter = ROOT / "tools" / "plot_compare.py"
        if plotter.exists():
            print("\nDrawing chart ...")
            subprocess.run([sys.executable, str(plotter),
                            str(RESULTS_DIR / "compare.csv"),
                            "-o", str(RESULTS_DIR / "compare_throughput.png")],
                           check=False)


if __name__ == "__main__":
    main()
