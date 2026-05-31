# Tối ưu hóa Pipe để Cải thiện Thông lượng IPC trong xv6

**Mã dự án:** 25236 — Hệ điều hành \[Multimedia\] 20252  
**Nhóm:** Nguyễn Sỹ Tuấn · Lê Huy Sơn · Vũ Tiến Long  
**Trường:** Điện – Điện tử, Đại học Bách Khoa Hà Nội

---

## Mô tả dự án

Dự án nghiên cứu và tối ưu hóa cơ chế **pipe IPC** trong hệ điều hành giáo dục
[xv6-riscv](https://github.com/mit-pdos/xv6-riscv), chạy trên kiến trúc RISC-V
(mô phỏng bằng QEMU).

Pipe baseline (v1) trong xv6 có thông lượng chỉ khoảng **~500 KB/s** do ba hạn chế
chính: copy từng byte (`copyin`/`copyout` × n lần), bộ đệm vòng chỉ 512 B, và gọi
`wakeup()` vô điều kiện sau mỗi lần ghi. Dự án triển khai **8 phiên bản tối ưu**
lũy tiến (v1–v8) và đo thông lượng bằng chương trình `pipebench`.

| Phiên bản | Tối ưu chính | Throughput @chunk=4096 B |
|-----------|-------------|--------------------------|
| v1 | Baseline (byte-by-byte, 512 B) | ~0.53 MB/s |
| v2 | Bulk copy (`copyin` cả khối) | ~1.74 MB/s |
| v3 | Ring of buffers (4 × 512 B) | ~5.72 MB/s |
| v4 | Multi-page: tách metadata + data (4 KB) | ~10.00 MB/s |
| v5 | Lazy wakeup (chỉ wake khi cần) | ~8.00 MB/s |
| v6 | Priority boost (scheduler) | ~10.00 MB/s |
| v7 | Cache-line alignment (SMP) | ~10.00 MB/s |
| v8 | All combined (v2–v7) | ~13.33 MB/s (**+25×**) |

---

## Cấu trúc thư mục

```
xv6-riscv/
├── kernel/
│   ├── pipe.c          # Triển khai pipe (v1–v8, chọn bằng PIPE_VERSION=N)
│   ├── proc.c          # Scheduler + priority boost (v6+)
│   ├── proc.h          # struct proc + trường priority (v6+)
│   └── ...             # Các file kernel khác của xv6
│
├── user/
│   ├── pipebench.c     # Đo throughput pipe theo chunk_bytes
│   ├── pipe_demo.c     # Demo IPC 2 chiều: nội dung + tốc độ
│   ├── pipe_chain.c    # Pipeline nhiều tiến trình nối tiếp
│   ├── pipe_fanout.c   # 1 writer → N readers
│   ├── pipe_pingpong.c # Ping-pong latency
│   ├── pipe_wc.c       # Word count qua pipe
│   └── ...
│
├── tools/
│   ├── plot_throughput.py   # Vẽ biểu đồ throughput từ ipctp.txt
│   ├── plot_compare.py      # So sánh nhiều phiên bản
│   └── run_ipc.py           # Tự động chạy benchmark
│
├── docs/
│   ├── book-riscv-rev3.pdf                          # xv6 book (MIT)
│   └── Abraham-Silberschatz-Operating-System-...pdf # OS Concepts
│
├── baocaohdh/
│   ├── baocao_tongket.tex   # Báo cáo tổng kết (LaTeX)
│   ├── baocao_tongket.pdf   # Báo cáo PDF
│   └── slide/
│       └── main.tex         # Slide thuyết trình (LaTeX/Beamer)
│
├── resultrun/
│   ├── CPUeq1/    # Ảnh benchmark CPUS=1 (v1–v8)
│   └── CPUeq2/    # Ảnh benchmark CPUS=2 (v7, v8)
│
└── Makefile
```

---

## Yêu cầu hệ thống

```bash
# Kiểm tra toolchain và QEMU
riscv64-unknown-elf-gcc --version   # hoặc riscv64-none-elf-gcc
qemu-system-riscv64 --version       # yêu cầu >= 5.1
```

---

## Build và chạy xv6

```bash
# Phiên bản mặc định (v7)
make qemu

# Chọn phiên bản cụ thể (N = 1..8)
make PIPE_VERSION=1 qemu     # baseline
make PIPE_VERSION=8 qemu     # fully optimized

# Chạy với 2 CPU (để thấy hiệu quả cache-line alignment của v7)
make PIPE_VERSION=7 CPUS=2 qemu

# Thoát QEMU: Ctrl-A rồi X
```

---

## Đo thông lượng pipe — `pipebench`

`pipebench` đo **throughput (B/s)** của pipe bằng cách truyền `total_bytes` qua
pipe trong các chunk `chunk_bytes`, dùng công thức:

```
Throughput = total_bytes × TICKS_PER_SEC / elapsed_ticks
```

### Cách chạy

```
# Trong xv6 shell sau khi khởi động QEMU:

pipebench                          # quét chunk_bytes (64 → 65536), total=4 MB
pipebench chunk [total_bytes]      # quét chunk, cố định total
pipebench total [chunk_bytes]      # quét total, cố định chunk
pipebench once [total] [chunk]     # đo 1 lần với total và chunk tùy chọn
```

### Ví dụ

```
$ pipebench
==== IPC throughput sweep: vary chunk_bytes ====
  total_bytes = 4194304
  chunk_bytes  |  B/s
  -------------|-------------------
  64           |  1023000
  128          |  1906501
  256          |  3495253
  ...
  4096         |  13981013
Saved results to ipctp.txt

$ pipebench chunk 1048576          # quét chunk với total = 1 MB
$ pipebench once 4194304 4096      # đo 1 lần: total=4MB, chunk=4096B
total=4194304 chunk=4096 => 13981013 B/s
```

Kết quả được lưu vào file `ipctp.txt` trong filesystem xv6. Để vẽ biểu đồ,
copy file ra host và chạy:

```bash
# Trên Linux host
python3 tools/plot_throughput.py ipctp.txt
```

---

## Demo trao đổi dữ liệu 2 chiều — `pipe_demo`

`pipe_demo` minh họa **IPC 2 chiều** giữa tiến trình cha (Parent) và tiến trình
con (Child) qua **2 pipe riêng biệt**:

```
Parent  ──[p2c pipe]──►  Child
Parent  ◄──[c2p pipe]──  Child
```

Chương trình chạy 2 phase:

| Phase | Vòng | Chunk | Mục đích |
|-------|------|-------|----------|
| Phase 1 | 10 | 64 B | In đầy đủ nội dung từng vòng (kiểm tra dữ liệu đúng) |
| Phase 2 | 800 | 4096 B | Đua tốc độ — không in từng vòng, chỉ in progress |

### Cách chạy

```
# Trong xv6 shell:

pipe_demo                   # chạy mặc định (800 vòng phase 2)
pipe_demo 200               # tùy chỉnh số vòng phase 2
```

### Ví dụ output

```
===========================================================
 pipe_demo  |  Bidirectional IPC  |  Content + Speed
===========================================================
 PHASE 1: in noi dung day du  (10 vong x 64 B)
 PHASE 2: dua toc do, KHONG in tung vong  (800 vong x 4096 B)
 v1: buffer 512B → 8 lan fill+sleep/wakeup/vong
 v7: buffer 4096B → 1 lan write, khong block/vong
===========================================================

--- PHASE 1: Content Verification ---

SEND #01 [64B]: "P>C r000001 t0003 AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
RECV #01 [64B]: "C>P r000001 t0003 ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ"
>> OK  cumul=128 B

...

--- PHASE 2: Speed Race (800 vong x 4096 B, '#' moi 50 vong) ---
  #  50 vong |     400 KB | t=500 ms
  # 100 vong |     800 KB | t=1000 ms
  ...

===========================================================
  KET QUA TONG HOP
===========================================================
  Phase 1 content : 10/10 vong dung
  Phase 2 rounds  : 800/800
  Tong du lieu    : 6400 KB  (6 MB)
  Thoi gian P2    : 6 ticks  (600 ms)
  Throughput      : 10922 KB/s  (10.6 MB/s)
===========================================================
```

### So sánh v1 vs v7/v8 với pipe_demo

```bash
# v1: buffer 512B → 8 lần fill+sleep mỗi vòng → chậm (~15 ms/vòng)
make PIPE_VERSION=1 qemu
# Trong xv6: pipe_demo

# v7: buffer 4KB → 1 lần write, không block → nhanh (~0.6 ms/vòng)
make PIPE_VERSION=7 qemu
# Trong xv6: pipe_demo
```
