#!/usr/bin/env python3
# =============================================================================
#  tools/run_ipc.py
# -----------------------------------------------------------------------------
#  Run the 4 IPC use-case programs inside xv6/QEMU and display results.
#  Compares two PIPE_VERSIONs side-by-side (default: v1 vs v8).
#
#  Programs run per version:
#    pipe_wc        — streaming text word-count
#    pipe_chain     — 3-stage transform pipeline
#    pipe_fanout    — fan-out 1 writer → N readers
#    pipe_pingpong  — bidirectional round-trip IPC
#
#  Usage:
#    python3 tools/run_ipc.py                  # compare v1 vs v8
#    python3 tools/run_ipc.py --versions 8     # only v8
#    python3 tools/run_ipc.py --versions 1,8   # explicit pair
#    python3 tools/run_ipc.py --skip-build     # assume fs.img already built
# =============================================================================

import argparse
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

VERSION_NAMES = {
    1: "v1 baseline (original xv6)",
    2: "v2 bulk copy",
    3: "v3 ring buffers",
    4: "v4 multi-page",
    5: "v5 lazy wakeup",
    6: "v6 priority boost",
    7: "v7 cache-line align",
    8: "v8 newpipe (all opts)",
}

# (command, description, timeout_seconds)
IPC_PROGRAMS = [
    ("pipe_wc",              "Streaming text word-count pipeline",   120),
    ("pipe_chain",           "3-stage XOR transform pipeline",       120),
    ("pipe_fanout",          "Fan-out: 1 writer -> 3 readers",       180),
    ("pipe_pingpong",        "Round-trip IPC (10000 rounds, 512 B)", 240),
]

# Regex to match "==== <title> ====" header line
HEADER_RE = re.compile(rb"====.*====")


# ============================================================================
#  Build helper
# ============================================================================
def run_make(version: int) -> None:
    print(f"\n[v{version}] make clean ...", flush=True)
    subprocess.run(["make", "clean"], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(f"[v{version}] make PIPE_VERSION={version} kernel/kernel fs.img ...",
          flush=True)
    res = subprocess.run(
        ["make", f"PIPE_VERSION={version}", "kernel/kernel", "fs.img"],
        cwd=ROOT, capture_output=True, text=True,
    )
    if res.returncode != 0:
        sys.stderr.write(res.stdout)
        sys.stderr.write(res.stderr)
        raise RuntimeError(f"build v{version} failed")
    print(f"[v{version}] build OK", flush=True)


# ============================================================================
#  Minimal QEMU driver (pty + select, no pexpect dependency)
# ============================================================================
class XV6Qemu:
    def __init__(self, log_path: Path | None = None):
        import pty
        master_fd, slave_fd = pty.openpty()
        self.master_fd = master_fd
        cmd = [
            "qemu-system-riscv64",
            "-machine", "virt", "-bios", "none",
            "-kernel", str(ROOT / "kernel" / "kernel"),
            "-m", "128M", "-smp", "1", "-nographic",
            "-global", "virtio-mmio.force-legacy=false",
            "-drive", f"file={ROOT / 'fs.img'},if=none,format=raw,id=x0",
            "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
        ]
        self.proc = subprocess.Popen(
            cmd,
            stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
            preexec_fn=os.setsid, close_fds=True,
        )
        os.close(slave_fd)
        self.buf = b""
        self.log_fd = open(log_path, "wb") if log_path else None

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
        if isinstance(pattern, str):
            pattern = re.compile(pattern.encode())
        elif isinstance(pattern, bytes):
            pattern = re.compile(pattern)
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = self._read_some(0.5)
            if chunk:
                self.buf += chunk
            m = pattern.search(self.buf)
            if m:
                consumed, self.buf = self.buf[: m.end()], self.buf[m.end():]
                return consumed
            if self.proc.poll() is not None:
                raise RuntimeError("qemu exited early")
        raise TimeoutError(f"timeout waiting for {pattern.pattern!r}")

    def sendline(self, s: str) -> None:
        os.write(self.master_fd, (s + "\n").encode())

    def quit(self) -> None:
        try:
            os.write(self.master_fd, b"\x01x")
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


# ============================================================================
#  Run one version: boot, run all 4 programs, collect output blocks
# ============================================================================
def run_version(version: int) -> dict[str, str]:
    """Boot QEMU for version, run all IPC programs, return {prog: output}."""
    log_path = RESULTS_DIR / f"ipc_v{version}.log"
    qemu = XV6Qemu(log_path=log_path)
    results = {}

    print(f"[v{version}] booting QEMU ...", flush=True)
    qemu.expect(rb"init: starting sh", timeout=60)
    qemu.expect(rb"\$ ", timeout=30)

    for (prog, desc, tmt) in IPC_PROGRAMS:
        print(f"[v{version}]   running {prog} ...", flush=True)
        qemu.sendline(prog)
        try:
            # Capture everything until next shell prompt
            raw = qemu.expect(rb"\$ ", timeout=tmt)
            text = raw.decode("utf-8", "replace")
            # Strip the command echo and trailing prompt
            lines = text.splitlines()
            # Drop the first line (echoed command) and last (prompt)
            body_lines = []
            started = False
            for line in lines:
                stripped = line.strip()
                if not started:
                    # Start collecting from the ==== header
                    if stripped.startswith("===="):
                        started = True
                        body_lines.append(stripped)
                else:
                    body_lines.append(stripped)
            results[prog] = "\n".join(body_lines).strip()
            print(f"[v{version}]   {prog}: done", flush=True)
        except TimeoutError:
            results[prog] = f"[TIMEOUT after {tmt}s]"
            print(f"[v{version}]   {prog}: TIMEOUT", flush=True)

    qemu.quit()
    print(f"[v{version}] QEMU exited\n", flush=True)
    return results


# ============================================================================
#  Display side-by-side comparison
# ============================================================================
def display_results(version_results: dict[int, dict[str, str]]) -> None:
    versions = sorted(version_results.keys())
    sep = "=" * 70

    print("\n" + sep)
    print("  IPC USE-CASE BENCHMARK RESULTS")
    print(sep)

    for (prog, desc, _) in IPC_PROGRAMS:
        print(f"\n{'─'*70}")
        print(f"  PROGRAM: {prog}  —  {desc}")
        print(f"{'─'*70}")
        for v in versions:
            vname = VERSION_NAMES.get(v, f"v{v}")
            out = version_results[v].get(prog, "[no output]")
            print(f"\n  [{vname}]")
            for line in out.splitlines():
                print(f"    {line}")

    # Extract throughput numbers for a summary table
    print(f"\n{'─'*70}")
    print("  THROUGHPUT SUMMARY  (KB/s)")
    print(f"{'─'*70}")
    tput_re = re.compile(r"throughput\s*:\s*\d+ B/s \((\d+) KB/s\)", re.IGNORECASE)
    ops_re  = re.compile(r"ops/sec\s*:\s*(\d+)", re.IGNORECASE)
    rtt_re  = re.compile(r"~latency/rtt\s*:\s*([\d.]+)", re.IGNORECASE)

    col_w = 22
    hdr = f"{'Program':<20}" + "".join(f"{VERSION_NAMES.get(v,'v'+str(v))[:col_w]:<{col_w}}" for v in versions)
    print(f"\n  {hdr}")
    print(f"  {'-'*(20+col_w*len(versions))}")

    for (prog, _, _) in IPC_PROGRAMS:
        row = f"{prog:<20}"
        for v in versions:
            out = version_results[v].get(prog, "")
            m = tput_re.search(out)
            if m:
                row += f"{m.group(1)+' KB/s':<{col_w}}"
            elif prog == "pipe_pingpong":
                m2 = ops_re.search(out)
                if m2:
                    row += f"{m2.group(1)+' ops/s':<{col_w}}"
                else:
                    row += f"{'?':<{col_w}}"
            else:
                row += f"{'?':<{col_w}}"
        print(f"  {row}")

    # RTT latency for pingpong
    print()
    for v in versions:
        out = version_results[v].get("pipe_pingpong", "")
        m = rtt_re.search(out)
        ops = ops_re.search(out)
        if m or ops:
            vname = VERSION_NAMES.get(v, f"v{v}")
            parts = []
            if ops:
                parts.append(f"ops/sec={ops.group(1)}")
            if m:
                parts.append(f"~rtt={m.group(1)} ms")
            print(f"  pingpong [{vname}]: {', '.join(parts)}")

    print(f"\n{sep}\n")

    # Save text report
    out_path = RESULTS_DIR / "ipc_results.txt"
    with out_path.open("w") as f:
        for v in versions:
            f.write(f"# version={v} {VERSION_NAMES.get(v,'')}\n")
            for (prog, _, _) in IPC_PROGRAMS:
                f.write(f"## {prog}\n")
                f.write(version_results[v].get(prog, "[no output]") + "\n\n")
    print(f"  Saved full results -> {out_path}")


# ============================================================================
#  Main
# ============================================================================
def main() -> None:
    ap = argparse.ArgumentParser(
        description="Run IPC use-case programs in xv6 QEMU")
    ap.add_argument("--versions", default="1,8",
                    help="comma-separated version list (default: 1,8)")
    ap.add_argument("--skip-build", action="store_true",
                    help="skip make (assume kernel/kernel and fs.img are ready)")
    args = ap.parse_args()

    versions = [int(x) for x in args.versions.split(",") if x.strip()]
    print(f"Versions to run: {versions}")

    all_results: dict[int, dict[str, str]] = {}
    for v in versions:
        if not args.skip_build:
            run_make(v)
        all_results[v] = run_version(v)

    display_results(all_results)


if __name__ == "__main__":
    main()
