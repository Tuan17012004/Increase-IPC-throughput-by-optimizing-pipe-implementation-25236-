// ============================================================================
//  pipeoverhead.c  -  Phân tích chi phí overhead (tổn hao) của pipe IPC
// ============================================================================
//
//  MỤC ĐÍCH:
//    Đo và phân tách thời gian tổn hao KHÔNG ĐÁNG (overhead) khi truyền dữ
//    liệu qua pipe trong xv6. Overhead bao gồm:
//      1. SYSCALL OVERHEAD: thời gian user→kernel→user cho mỗi write()/read()
//      2. SLEEP/WAKEUP OVERHEAD: context switch khi buffer đầy/rỗng
//      3. LOCK OVERHEAD: spinlock acquire/release mỗi lần thao tác pipe
//    Phần còn lại là thời gian HỮU ÍCH: sao chép dữ liệu thật sự (copyin/
//    copyout giữa user buffer và pipe buffer).
//
//  PHƯƠNG PHÁP:
//    Bước 1 - CALIBRATION (đo chuẩn):
//      Đo chi phí trung bình của 1 syscall bằng cách gọi getpid() nhiều lần
//      (getpid là syscall rẻ nhất, chỉ trả về PID, không I/O).
//
//    Bước 2 - BENCHMARK (đo thực tế):
//      Với mỗi giá trị chunk_bytes, chạy pipe transfer giống pipebench,
//      nhưng ĐẾM số lần gọi write() thực tế (n_write_calls).
//
//    Bước 3 - PHÂN TÍCH:
//      Tính toán phân tách overhead:
//
//      ┌──────────────────── TỔNG THỜI GIAN (total_us) ────────────────────┐
//      │                                                                    │
//      │  ┌── Syscall OH ──┐  ┌── Sleep/Wake OH ──┐  ┌── Copy (hữu ích) ──┤
//      │  │ N_sys × t_sys  │  │  total - sys - ?  │  │  thực sự copy data │
//      │  └────────────────┘  └───────────────────┘  └─────────────────────┘
//      │                                                                    │
//      └────────────────────────────────────────────────────────────────────┘
//
//      Trong đó:
//        N_syscalls    = n_write_calls + n_read_calls (≈ 2 × total/chunk)
//        t_syscall     = thời gian 1 syscall (từ calibration)
//        syscall_oh    = N_syscalls × t_syscall
//        N_sleeps      ≈ 2 × total_bytes / PIPESIZE
//                        (writer ngủ khi buffer đầy, reader ngủ khi rỗng)
//        other_oh      = total - syscall_oh
//                        (bao gồm sleep/wakeup + lock + byte-copy overhead)
//        overhead_pct  = (total - useful_copy) / total × 100
//
//  QUAN HỆ VỚI chunk_bytes:
//    ┌────────────────────────────────────────────────────────────────────┐
//    │  chunk_bytes NHỎ  →  NHIỀU syscall  →  syscall_oh LỚN           │
//    │                      (total/64 = 65536 calls cho 4MB)            │
//    │                                                                   │
//    │  chunk_bytes LỚN  →  ÍT syscall    →  syscall_oh NHỎ            │
//    │                      (total/4096 = 1024 calls cho 4MB)           │
//    │                                                                   │
//    │  NHƯNG: sleep/wakeup KHÔNG PHỤ THUỘC chunk_bytes!                │
//    │         N_sleeps ≈ total/PIPESIZE = HẰNG SỐ                      │
//    │         → Khi chunk lớn, sleep/wakeup trở thành bottleneck chính │
//    └────────────────────────────────────────────────────────────────────┘
//
//  ĐIỀU NÀY CHỨNG MINH: Cần TĂNG PIPESIZE (cơ chế 2) để giảm sleep/wakeup!
//
// ----------------------------------------------------------------------------
//  OUTPUT:
//    Console: bảng chi tiết overhead cho từng chunk_bytes
//    File overhead.txt: dữ liệu để vẽ biểu đồ phân tách overhead
// ============================================================================

#include "../kernel/types.h"
#include "../kernel/fcntl.h"
#include "user.h"

// ----- Hằng số cấu hình ----------------------------------------------------
#define TICKS_PER_SEC      10           // 1 tick ≈ 100ms trong xv6

#define DEFAULT_TOTAL      (4 * 1024 * 1024)   // 4 MB tổng dữ liệu

// PHẢI KHỚP với PIPE_TOTAL trong kernel/pipe.c (DATA_PAGES × PGSIZE)
// Dùng để ước tính số lần sleep/wakeup.
// Hiện tại: 1 data page × 4096B = 4096B
#define PIPESIZE_KERN      4096

#define OUTPUT_FILE        "overhead.txt"

// Số lần lặp để đo chi phí 1 syscall. Cần đủ lớn vì xv6 chỉ đo được
// theo tick (100ms). Với 100000 lần getpid(), thường mất ~5-20 ticks.
#define CALIB_ITERS        100000

// ----- Cấu hình quét chunk_bytes -------------------------------------------
// Thay vì dùng lũy thừa 2 (64, 128, 256,...), quét tuyến tính với bước
// nhảy nhỏ (128B) để biểu đồ mượt hơn và thấy rõ xu hướng thay đổi.
//   Dải quét: 128, 256, 384, 512, ..., 4096  (bước 128, tổng 32 điểm)
#define CHUNK_START  128    // giá trị chunk nhỏ nhất
#define CHUNK_END    4096   // giá trị chunk lớn nhất
#define CHUNK_STEP   128    // bước nhảy giữa các lần đo

// Hàm min đơn giản
static int
imin(int a, int b)
{
  return a < b ? a : b;
}

// ============================================================================
//  itoa() - chuyển số nguyên → chuỗi (xv6 không có sprintf)
// ============================================================================
static void
itoa(int n, char *buf)
{
  char tmp[16];
  int i = 0, j;
  if(n == 0){
    buf[0] = '0';
    buf[1] = 0;
    return;
  }
  if(n < 0){
    *buf++ = '-';
    n = -n;
  }
  while(n > 0){
    tmp[i++] = '0' + (n % 10);
    n /= 10;
  }
  for(j = 0; j < i; j++)
    buf[j] = tmp[i - 1 - j];
  buf[i] = 0;
}

// Ghi chuỗi ra fd
static void
write_str(int fd, char *s)
{
  write(fd, s, strlen(s));
}

// Ghi số nguyên ra fd
static void
write_int(int fd, int n)
{
  char buf[16];
  itoa(n, buf);
  write_str(fd, buf);
}

// ============================================================================
//  calibrate_syscall() - đo chi phí trung bình 1 syscall (microseconds)
// ============================================================================
//
//  PHƯƠNG PHÁP:
//    Gọi getpid() lặp CALIB_ITERS lần, đo tổng ticks, quy đổi:
//
//      total_us = elapsed_ticks × (1000000 / TICKS_PER_SEC)
//      us_per_syscall = total_us / CALIB_ITERS
//
//  TẠI SAO DÙNG getpid()?
//    - Đây là syscall "rẻ nhất" trong xv6: chỉ đọc proc->pid và trả về.
//    - KHÔNG có I/O, KHÔNG acquire lock, KHÔNG sleep.
//    - Tuy nhiên vẫn phải trải qua đầy đủ:
//        ecall → uservec → usertrap → syscall → sys_getpid → userret
//    - Nên nó đo đúng OVERHEAD thuần túy của mode-switch (user↔kernel).
//
//  KẾT QUẢ ĐIỂN HÌNH (QEMU, rv64):
//    ~1-5 μs/syscall (phụ thuộc tốc độ host)
//
// ============================================================================
static int
calibrate_syscall(void)
{
  int i;
  int start, end, elapsed;

  printf("Calibrating syscall overhead (%d iterations)...\n", CALIB_ITERS);

  start = uptime();
  for(i = 0; i < CALIB_ITERS; i++){
    getpid();   // syscall rẻ nhất, chỉ đo mode-switch cost
  }
  end = uptime();

  elapsed = end - start;
  if(elapsed <= 0)
    elapsed = 1;

  // Quy đổi:
  //   elapsed ticks × (1,000,000 μs / TICKS_PER_SEC) = total_us
  //   us_per_syscall = total_us / CALIB_ITERS
  //
  // Để tránh overflow khi nhân, dùng phép chia trước:
  //   us_per_syscall = (elapsed * 1000000) / (CALIB_ITERS * TICKS_PER_SEC)
  //
  // Với elapsed ~10 ticks, CALIB_ITERS = 100000:
  //   = (10 * 1000000) / (100000 * 10) = 10000000 / 1000000 = 10 μs
  int us_per_syscall = (elapsed * 1000000) / (CALIB_ITERS * TICKS_PER_SEC);

  printf("  Elapsed: %d ticks (%d ms)\n", elapsed, elapsed * 1000 / TICKS_PER_SEC);
  printf("  Per syscall: ~%d us\n", us_per_syscall);

  return us_per_syscall;
}

// ============================================================================
//  Cấu trúc kết quả benchmark - trả về nhiều metric từ 1 lần đo
// ============================================================================
struct overhead_result {
  int total_ticks;         // tổng thời gian đo (ticks)
  int total_us;            // tổng thời gian đo (microseconds)
  int throughput_bps;      // throughput (bytes/sec)
  int n_write_calls;       // số lần gọi write() THỰC TẾ
  int n_read_calls;        // số lần gọi read() (ước tính = n_write_calls)
  int n_total_syscalls;    // tổng syscalls = write + read
  int n_sleeps;            // số lần sleep/wakeup (ước tính)
  int syscall_overhead_us; // thời gian tổn hao syscall (μs)
  int other_overhead_us;   // thời gian tổn hao sleep/wakeup + lock + khác (μs)
  int overhead_pct;        // tỉ lệ overhead / tổng (%)
};

// ============================================================================
//  bench_overhead() - chạy 1 phép đo và phân tách overhead
// ============================================================================
//
//  GIỐNG bench() trong pipebench.c nhưng:
//    1. ĐẾM số lần gọi write() thực tế (n_write_calls)
//    2. ƯỚC TÍNH số lần sleep/wakeup
//    3. TÍNH overhead dựa trên calibration
//
//  FLOW:
//    parent (writer)                        child (reader)
//        |                                      |
//    pipe() + fork()                            |
//        |                                      |
//    start = uptime()                           |
//    while(sent < total) {                  while(got < total) {
//      write() → ĐẾM n_write_calls           read()
//    }                                      }
//    close + wait + end = uptime()           exit(0)
//        |
//    Tính toán overhead
//
// ============================================================================
static int
bench_overhead(int total_bytes, int chunk_bytes,
               int us_per_syscall, struct overhead_result *res)
{
  int p[2];
  int pid;
  char *buf;
  int sent = 0;
  int write_calls = 0;

  buf = malloc(chunk_bytes);
  if(buf == 0)
    return -1;
  memset(buf, 'a', chunk_bytes);

  if(pipe(p) < 0){
    free(buf);
    return -1;
  }

  pid = fork();
  if(pid < 0){
    close(p[0]);
    close(p[1]);
    free(buf);
    return -1;
  }

  // ==== CHILD = READER =====================================================
  if(pid == 0){
    int got = 0, n;
    close(p[1]);
    while(got < total_bytes){
      n = read(p[0], buf, imin(chunk_bytes, total_bytes - got));
      if(n <= 0)
        break;
      got += n;
    }
    close(p[0]);
    exit(0);
  }

  // ==== PARENT = WRITER ====================================================
  close(p[0]);
  int start_ticks = uptime();

  while(sent < total_bytes){
    int n = write(p[1], buf, imin(chunk_bytes, total_bytes - sent));
    if(n <= 0){
      close(p[1]);
      wait(0);
      free(buf);
      return -1;
    }
    sent += n;
    write_calls++;   // ← ĐẾM số lần gọi write() thực tế
  }
  close(p[1]);
  wait(0);
  int end_ticks = uptime();

  int elapsed = end_ticks - start_ticks;
  if(elapsed <= 0)
    elapsed = 1;
  free(buf);

  // ==== TÍNH TOÁN CÁC METRIC OVERHEAD =====================================
  //
  // 1. Tổng thời gian
  res->total_ticks = elapsed;
  res->total_us = (elapsed * 1000000) / TICKS_PER_SEC;
  res->throughput_bps = (total_bytes * TICKS_PER_SEC) / elapsed;

  // 2. Số syscall
  //    - n_write_calls: đếm thực tế ở trên
  //    - n_read_calls: reader gọi read() khoảng bằng writer (≈ total/chunk)
  //      Thực tế có thể hơi khác vì read() trả về ≤ chunk bytes,
  //      nhưng xấp xỉ tốt.
  res->n_write_calls = write_calls;
  res->n_read_calls = write_calls;   // xấp xỉ
  res->n_total_syscalls = write_calls * 2;

  // 3. Số lần sleep/wakeup (ước tính)
  //    Writer ngủ mỗi khi buffer đầy: ~total_bytes/PIPESIZE lần
  //    Reader ngủ mỗi khi buffer rỗng: tương tự
  //    Tổng ≈ 2 × total_bytes / PIPESIZE
  res->n_sleeps = (total_bytes * 2) / PIPESIZE_KERN;

  // 4. Syscall overhead (μs)
  //    Mỗi write()/read() phải trải qua:
  //      ecall → uservec → usertrap → syscall() → sys_write/read
  //      → filewrite/read → pipewrite/read → ... → userret
  //    Chi phí mode-switch thuần túy ≈ us_per_syscall (từ calibration)
  //    × số lần gọi.
  res->syscall_overhead_us = res->n_total_syscalls * us_per_syscall;

  // 5. Overhead còn lại = total - syscall_overhead
  //    Bao gồm: sleep/wakeup context switch + spinlock contention
  //    + byte-by-byte copy overhead (hiện tại pipe.c copy từng byte)
  res->other_overhead_us = res->total_us - res->syscall_overhead_us;
  if(res->other_overhead_us < 0)
    res->other_overhead_us = 0;  // phòng vệ khi calibration không chính xác

  // 6. Tỉ lệ overhead (%)
  //    overhead = tổng thời gian - thời gian hữu ích (copy data thuần)
  //    Ở đây lấy syscall_overhead_us làm phần overhead đo được trực tiếp,
  //    phần còn lại (other) cũng chủ yếu là overhead (sleep + lock).
  //    → overhead ≈ gần 100% vì hầu hết thời gian KHÔNG phải copy thuần.
  //
  //    Để đơn giản, tính: overhead_pct = syscall_oh / total × 100
  //    Phần other_oh cũng là overhead nhưng khó tách riêng copy thuần.
  if(res->total_us > 0)
    res->overhead_pct = (res->syscall_overhead_us * 100) / res->total_us;
  else
    res->overhead_pct = 0;

  return 0;
}

// ============================================================================
//  sweep_overhead() - quét chunk_bytes, in bảng overhead chi tiết
// ============================================================================
//
//  OUTPUT CONSOLE (ví dụ):
//
//    chunk  | total_us | N_sys  | sys_oh_us | N_sleep | other_oh | sys_oh%
//    -------+----------+--------+-----------+---------+----------+--------
//      64   | 8500000  | 131072 |  1310720  |  16384  | 7189280  |  15%
//     128   | 5200000  |  65536 |   655360  |  16384  | 4544640  |  12%
//     256   | 3800000  |  32768 |   327680  |  16384  | 3472320  |   8%
//     512   | 3200000  |  16384 |   163840  |  16384  | 3036160  |   5%
//    1024   | 2900000  |   8192 |    81920  |  16384  | 2818080  |   2%
//    2048   | 2700000  |   4096 |    40960  |  16384  | 2659040  |   1%
//    4096   | 2600000  |   2048 |    20480  |  16384  | 2579520  |   0%
//
//  NHẬN XÉT QUAN TRỌNG TỪ BẢNG:
//    → N_sleep = 16384 LÀ HẰNG SỐ bất kể chunk_bytes!
//      (vì N_sleep = 2 × 4MB / 512 = 16384)
//    → Khi chunk_bytes tăng: sys_oh GIẢM, nhưng other_oh (chủ yếu là
//      sleep/wakeup) GẦN NHƯ KHÔNG ĐỔI → đây là BOTTLENECK!
//    → GIẢI PHÁP: Tăng PIPESIZE để giảm N_sleep!
//
// ============================================================================
static int
sweep_overhead(int total_bytes, int us_per_syscall, int fd)
{
  int chunk;
  struct overhead_result res;
  int n_points = (CHUNK_END - CHUNK_START) / CHUNK_STEP + 1;

  // In header
  printf("\n==== PIPE OVERHEAD ANALYSIS (sweep chunk_bytes) ====\n");
  printf("  total_bytes  = %d\n", total_bytes);
  printf("  PIPESIZE     = %d (kernel)\n", PIPESIZE_KERN);
  printf("  us/syscall   = %d (calibrated)\n", us_per_syscall);
  printf("  chunk range  = %d -> %d, step %d (%d points)\n\n",
         CHUNK_START, CHUNK_END, CHUNK_STEP, n_points);

  printf("  %-7s | %-10s | %-7s | %-10s | %-7s | %-10s | %-6s\n",
         "chunk", "total_us", "N_sys", "sys_oh_us", "N_sleep", "other_oh", "sys%");
  printf("  -------+------------+---------+------------+---------+");
  printf("------------+------\n");

  // Ghi header vào file
  write_str(fd, "# PIPE OVERHEAD ANALYSIS\n");
  write_str(fd, "# total_bytes=");
  write_int(fd, total_bytes);
  write_str(fd, " PIPESIZE=");
  write_int(fd, PIPESIZE_KERN);
  write_str(fd, " us_per_syscall=");
  write_int(fd, us_per_syscall);
  write_str(fd, " chunk_step=");
  write_int(fd, CHUNK_STEP);
  write_str(fd, "\n");
  write_str(fd, "# chunk total_us N_syscalls syscall_oh_us N_sleeps other_oh_us syscall_oh_pct throughput_bps\n");

  // Quét tuyến tính: chunk = 128, 256, 384, ..., 4096 (bước 128)
  for(chunk = CHUNK_START; chunk <= CHUNK_END; chunk += CHUNK_STEP){
    if(bench_overhead(total_bytes, chunk, us_per_syscall, &res) < 0){
      printf("  %-7d | FAILED\n", chunk);
      continue;
    }

    // In bảng console
    printf("  %-7d | %-10d | %-7d | %-10d | %-7d | %-10d | %-3d%%\n",
           chunk,
           res.total_us,
           res.n_total_syscalls,
           res.syscall_overhead_us,
           res.n_sleeps,
           res.other_overhead_us,
           res.overhead_pct);

    // Ghi record vào file (1 dòng per chunk_bytes)
    write_int(fd, chunk);                   write_str(fd, " ");
    write_int(fd, res.total_us);            write_str(fd, " ");
    write_int(fd, res.n_total_syscalls);    write_str(fd, " ");
    write_int(fd, res.syscall_overhead_us); write_str(fd, " ");
    write_int(fd, res.n_sleeps);            write_str(fd, " ");
    write_int(fd, res.other_overhead_us);   write_str(fd, " ");
    write_int(fd, res.overhead_pct);        write_str(fd, " ");
    write_int(fd, res.throughput_bps);      write_str(fd, "\n");
  }

  // In tóm tắt
  printf("\n");
  printf("  ┌──────────────────────────────────────────────────────────────┐\n");
  printf("  │  NHAN XET:                                                  │\n");
  printf("  │  - N_sleep = %d (KHONG DOI khi tang chunk_bytes)       │\n",
         (total_bytes * 2) / PIPESIZE_KERN);
  printf("  │    vi N_sleep = 2 * total / PIPESIZE = hang so              │\n");
  printf("  │  - Khi chunk lon, syscall_oh giam nhung other_oh (sleep/    │\n");
  printf("  │    wakeup) van lon => DAY la BOTTLENECK!                    │\n");
  printf("  │  - GIAI PHAP: Tang PIPESIZE (512 -> 4096) de giam N_sleep  │\n");
  printf("  │    tu %d xuong %d (giam 8x)              │\n",
         (total_bytes * 2) / PIPESIZE_KERN,
         (total_bytes * 2) / 4096);
  printf("  └──────────────────────────────────────────────────────────────┘\n");

  return 0;
}

// ============================================================================
//  main()
// ============================================================================
int
main(int argc, char *argv[])
{
  int total_bytes = DEFAULT_TOTAL;
  int us_per_syscall;
  int fd;

  // Parse argv tùy chọn
  if(argc >= 2){
    int v = atoi(argv[1]);
    if(v > 0)
      total_bytes = v;
    else {
      fprintf(2, "usage: pipeoverhead [total_bytes]\n");
      fprintf(2, "  default total_bytes = %d (4MB)\n", DEFAULT_TOTAL);
      exit(1);
    }
  }

  printf("============================================================\n");
  printf("  PIPE OVERHEAD ANALYZER\n");
  printf("  Phan tach thoi gian ton hao khi truyen du lieu qua pipe\n");
  printf("============================================================\n\n");

  // ---- BƯỚC 1: Calibration -------------------------------------------------
  us_per_syscall = calibrate_syscall();
  printf("\n");

  // ---- BƯỚC 2: Mở file output ----------------------------------------------
  fd = open(OUTPUT_FILE, O_CREATE | O_WRONLY | O_TRUNC);
  if(fd < 0){
    fprintf(2, "pipeoverhead: cannot open %s\n", OUTPUT_FILE);
    exit(1);
  }

  // ---- BƯỚC 3: Sweep chunk_bytes -------------------------------------------
  sweep_overhead(total_bytes, us_per_syscall, fd);

  close(fd);
  printf("\nSaved results to %s\n", OUTPUT_FILE);
  exit(0);
}
