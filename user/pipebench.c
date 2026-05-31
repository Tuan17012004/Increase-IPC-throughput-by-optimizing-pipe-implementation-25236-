// ============================================================================
//  pipebench.c  -  Đo throughput (thông lượng) của xv6 pipe IPC
// ============================================================================
//
//  MỤC ĐÍCH:
//    Đây là chương trình user-space chạy trong xv6 dùng để đo định lượng
//    hiệu năng của cơ chế Inter-Process Communication (IPC) bằng pipe.
//    Khác với pipetest.c (chỉ chạy 1 cấu hình duy nhất), pipebench QUÉT
//    (sweep) qua nhiều giá trị tham số, ghi kết quả ra file để vẽ biểu đồ.
//
//  TẠI SAO CẦN PROGRAM NÀY?
//    Tuần 4 chúng tôi đã viết pipetest đo throughput cho 1 cặp
//    (total_bytes, chunk_bytes). Nhưng để đánh giá tối ưu (Tuần 5), cần
//    so sánh trên NHIỀU chunk size khác nhau (64B, 128B,..., 4KB) để
//    thấy rõ điểm nghẽn cổ chai (bottleneck) của copy-từng-byte. Một
//    biểu đồ thông lượng theo chunk size là bằng chứng định lượng cho
//    việc tối ưu thành công.
//
//  CÔNG THỨC THÔNG LƯỢNG:
//      Throughput (B/s) = total_bytes / time_seconds
//    Trong xv6, thời gian đo bằng "tick" (1 tick ≈ 100ms ⇒ 10 ticks/s):
//      time_seconds = elapsed_ticks / TICKS_PER_SEC
//    ⇒ Throughput = total_bytes * TICKS_PER_SEC / elapsed_ticks
//
// ----------------------------------------------------------------------------
//  QUAN HỆ GIỮA total_bytes VÀ chunk_bytes
// ----------------------------------------------------------------------------
//  total_bytes = TỔNG dữ liệu cần truyền qua pipe trong 1 lần đo.
//  chunk_bytes = kích thước mỗi lần gọi write()/read() (1 syscall).
//
//  Bên writer phải gọi write() khoảng N = total_bytes / chunk_bytes lần,
//  bên reader phải gọi read() khoảng N lần để hoàn thành phép đo.
//
//  Ví dụ: total_bytes = 1024, chunk_bytes = 256 ⇒ N = 4 lần write/read.
//
//      Buffer user-space (4 chunks × 256B = 1024B)
//      [chunk0 256B][chunk1 256B][chunk2 256B][chunk3 256B]
//          |             |             |             |
//          v             v             v             v
//      write #1      write #2      write #3      write #4
//          \             \             \             /
//           \             \             \           /
//            \             \             \         /
//             v             v             v       v
//        +----------------------------------------+
//        |   PIPE BUFFER trong KERNEL (≈4KB)      |   ← circular buffer
//        +----------------------------------------+
//             |             |             |       |
//             v             v             v       v
//        read #1       read #2       read #3   read #4
//             |             |             |       |
//             v             v             v       v
//      [recv0 256B] [recv1 256B] [recv2 256B] [recv3 256B]
//      Buffer user-space của reader (cũng 1024B sau 4 lần read)
//
//  CHUNK_BYTES NHỎ ⇒ NHIỀU SYSCALL ⇒ THROUGHPUT THẤP:
//      Mỗi write() / read() đều phải:
//        - chuyển mode user → kernel (trap)
//        - acquire(&pi->lock)   spinlock
//        - copyin/copyout giữa user space và pipe buffer
//        - release(&pi->lock)
//        - chuyển mode kernel → user
//      Phần overhead này ~ vài microseconds, KHÔNG phụ thuộc chunk_bytes.
//      Nếu chunk = 64B thì overhead/byte = overhead/64 (cao).
//      Nếu chunk = 4096B thì overhead/byte = overhead/4096 (thấp).
//
// ----------------------------------------------------------------------------
//  CƠ CHẾ CHUNK GHI/ĐỌC QUA PIPE (CIRCULAR BUFFER)
// ----------------------------------------------------------------------------
//  Pipe buffer trong kernel là circular buffer kích thước PIPESIZE (~4096B):
//
//          nread (tail, đọc kế tiếp)         nwrite (head, ghi kế tiếp)
//                  |                                  |
//                  v                                  v
//        +---+---+---+---+---+---+---+---+---+---+---+---+---+
//        | A | B | C | D | E | . | . | . | . | . | . | . | . |
//        +---+---+---+---+---+---+---+---+---+---+---+---+---+
//        0   1   2   3   4   5  ...                       PIPESIZE-1
//        \---------- đã ghi, chưa đọc ----------/
//        \------------------- vùng trống -----------------/
//
//  KHI WRITER GHI 1 CHUNK (giả sử chunk = 4):
//    1. acquire(&pipe->lock)
//    2. Nếu (nwrite - nread) == PIPESIZE  →  buffer ĐẦY:
//          wakeup(&nread); sleep(&nwrite); ← NGỦ chờ reader đọc bớt
//          (chính lúc này context switch làm tốn thời gian)
//    3. Bulk copy chunk vào data[nwrite % PIPESIZE..]:
//          data[nwrite+0] = X
//          data[nwrite+1] = Y
//          data[nwrite+2] = Z
//          data[nwrite+3] = W
//       Sau đó nwrite += chunk.
//    4. wakeup(&nread)  ← đánh thức reader nếu nó đang ngủ
//    5. release(&pipe->lock)
//
//  KHI READER ĐỌC 1 CHUNK:
//    1. acquire(&pipe->lock)
//    2. Nếu nread == nwrite  →  buffer RỖNG:
//          sleep(&nread)  ← NGỦ chờ writer ghi
//    3. Bulk copy chunk từ data[nread % PIPESIZE..] ra user buffer.
//          nread += chunk
//    4. wakeup(&nwrite)  ← đánh thức writer nếu đang ngủ
//    5. release(&pipe->lock)
//
//  KỊCH BẢN STEADY-STATE (writer chạy nhanh hơn reader):
//      [W ghi 1 chunk] → [W ghi 1 chunk] → ... → buffer FULL
//        ↓
//      [W sleep] - [R wakeup, đọc dữ liệu] - [R wakeup W]
//        ↓
//      [W tiếp tục ghi] → ...
//
//  SỐ LẦN sleep/wakeup ≈ total_bytes / PIPESIZE ⇒ KHÔNG phụ thuộc chunk_bytes.
//  ⇒ Khi chunk_bytes tăng, số syscall giảm nhưng số lần ngủ-thức không đổi
//    ⇒ ROI (return on investment) chính khi tăng chunk là giảm overhead
//    syscall, không phải giảm sleep/wakeup.
//
// ----------------------------------------------------------------------------
//  DÒNG ĐỜI 1 LẦN bench()
// ----------------------------------------------------------------------------
//
//      total_bytes = 1024, chunk_bytes = 256
//
//                 PARENT (writer)                CHILD (reader)
//                       |                              |
//      pipe()  ─────────┤                              |
//                       │                              |
//      fork()  ─────────┴─── tạo child ───────────────►|
//                       │                              |
//                  close(p[0])                    close(p[1])
//                       │                              |
//      start_ticks = uptime()                          |
//                       │                              |
//                  ┌────▼─────┐                  ┌─────▼─────┐
//        loop {    │ write 256│ ─── chunk #1 ──► │ read 256  │
//                  └────┬─────┘                  └─────┬─────┘
//                       │                              │
//                  ┌────▼─────┐                  ┌─────▼─────┐
//                  │ write 256│ ─── chunk #2 ──► │ read 256  │
//                  └────┬─────┘                  └─────┬─────┘
//                       │                              │
//                  ┌────▼─────┐                  ┌─────▼─────┐
//                  │ write 256│ ─── chunk #3 ──► │ read 256  │
//                  └────┬─────┘                  └─────┬─────┘
//                       │                              │
//                  ┌────▼─────┐                  ┌─────▼─────┐
//                  │ write 256│ ─── chunk #4 ──► │ read 256  │
//                  └────┬─────┘                  └─────┬─────┘
//        }              │                              │
//                  close(p[1])  ──► EOF báo cho reader │
//                       │                              │
//                  wait(child) ◄──── exit(0) ──────────┘
//                       │
//      end_ticks = uptime()
//                       │
//      elapsed = end - start
//      return total_bytes * TICKS_PER_SEC / elapsed
//
// ----------------------------------------------------------------------------
//  CHI TIẾT: write() VÀ read() ĐƯỢC GỌI NHƯ THẾ NÀO?
// ----------------------------------------------------------------------------
//  Khi bench() gọi write(p[1], buf, chunk_bytes), thực chất đây là CHẶNG
//  ĐẦU của một chuỗi dài nhiều tầng đi qua biên user/kernel:
//
//      User space (pipebench.c)
//      ─────────────────────────────────────────────────────────
//                         write(fd, buf, n)
//                                │
//                                ▼  (user/usys.S - tạo bởi usys.pl)
//                         li a7, SYS_write
//                         ecall                ◄── trap vào kernel
//                                │
//      ─────────────────────────────────────────────────────────
//      Kernel space
//                                ▼  (kernel/trampoline.S - uservec)
//                         lưu thanh ghi user
//                                ▼  (kernel/trap.c - usertrap)
//                         nhận ra đây là syscall
//                                ▼  (kernel/syscall.c - syscall)
//                         đọc a7 = SYS_write
//                         tra bảng syscalls[]
//                                ▼  (kernel/sysfile.c - sys_write)
//                         lấy fd, addr (uint64), n
//                         gọi filewrite(f, addr, n)
//                                ▼  (kernel/file.c - filewrite)
//                         f->type == FD_PIPE ?
//                         có → gọi pipewrite(f->pipe, addr, n)
//                                ▼  (kernel/pipe.c - pipewrite)
//                         đây mới là phần làm việc thật!
//                                ▼
//                         acquire(&pi->lock)
//                         while(i < n)
//                            copyin từ user buffer vào pi->data[]
//                         release(&pi->lock)
//                         return số byte đã ghi
//                                │
//      ◄────── trở về user qua trapret/userret ──────────────────
//                                │
//      User space: write() trả về số byte
//
//  TƯƠNG TỰ CHO read(): user write() → ecall → usertrap → syscall →
//  sys_read → fileread → piperead → ... → trả về user.
//
// ----------------------------------------------------------------------------
//  pipewrite() - LÀM VIỆC THẬT (kernel/pipe.c)
// ----------------------------------------------------------------------------
//  Đây là code THỰC SỰ chạy mỗi khi bench() gọi write():
//
//      int pipewrite(struct pipe *pi, uint64 addr, int n) {
//          int i = 0;
//          struct proc *pr = myproc();
//
//          acquire(&pi->lock);                       ← (1) chiếm lock
//          while(i < n) {
//              if(pi->readopen == 0 || killed(pr)){  ← (2) reader đã đóng?
//                  release(&pi->lock); return -1;
//              }
//              if(pi->nwrite == pi->nread + PIPESIZE){       ← (3) buffer FULL
//                  wakeup(&pi->nread);               ← đánh thức reader
//                  sleep(&pi->nwrite, &pi->lock);    ← writer đi NGỦ
//              } else {
//                  // (4) bulk copy: ghi nhiều byte/lần (optimization Tuần 5)
//                  uint free_slots = PIPESIZE - (pi->nwrite - pi->nread);
//                  uint head_slot  = pi->nwrite % PIPESIZE;
//                  uint till_wrap  = PIPESIZE - head_slot;
//                  int to_copy = min(n - i, free_slots, till_wrap);
//                  copyin(pr->pagetable, &pi->data[head_slot],
//                         addr + i, to_copy);
//                  pi->nwrite += to_copy;
//                  i          += to_copy;
//              }
//          }
//          wakeup(&pi->nread);                       ← (5) báo reader có data
//          release(&pi->lock);
//          return i;
//      }
//
//  GIẢI THÍCH 5 BƯỚC:
//    (1) acquire spinlock - đảm bảo chỉ 1 tiến trình thay đổi pipe lúc 1 thời
//        điểm (mutex). xv6 dùng spin-lock vì critical section ngắn.
//    (2) Phòng vệ: nếu reader đã close hết hoặc bị kill → ghi vào pipe vô
//        ích → trả -1 (broken pipe).
//    (3) Buffer đầy ⇒ KHÔNG còn slot trống ⇒ writer phải đi ngủ. Trước khi
//        ngủ wakeup reader vì có thể reader đang nhàn rỗi. sleep() tự động
//        release lock tạm thời để reader có thể acquire.
//    (4) Phần "ngon nhất": bulk copy. Trước đây code copy 1 byte/lần × N
//        lần (rất chậm). Bản tối ưu Tuần 5 tính toán số byte liên tiếp có
//        thể copy mà không vượt qua biên circular buffer, rồi gọi copyin
//        một lần duy nhất.
//    (5) Sau khi ghi xong → wakeup reader nếu nó đang ngủ chờ data.
//
// ----------------------------------------------------------------------------
//  piperead() - ĐỐI XỨNG VỚI pipewrite()
// ----------------------------------------------------------------------------
//      int piperead(struct pipe *pi, uint64 addr, int n) {
//          acquire(&pi->lock);
//          while(pi->nread == pi->nwrite && pi->writeopen){    ← buffer EMPTY
//              if(killed(pr)){ release; return -1; }
//              sleep(&pi->nread, &pi->lock);                   ← reader NGỦ
//          }
//          for(i = 0; i < n; ){
//              if(pi->nread == pi->nwrite) break;       ← hết data, dừng
//              // bulk copy ra user space
//              uint avail     = pi->nwrite - pi->nread;
//              uint tail_slot = pi->nread % PIPESIZE;
//              uint till_wrap = PIPESIZE - tail_slot;
//              int to_copy = min(n - i, avail, till_wrap);
//              copyout(pr->pagetable, addr + i,
//                      &pi->data[tail_slot], to_copy);
//              pi->nread += to_copy;
//              i         += to_copy;
//          }
//          wakeup(&pi->nwrite);                          ← báo writer có chỗ
//          release(&pi->lock);
//          return i;
//      }
//
//  ĐIỂM KHÁC:
//    - Reader ngủ khi buffer RỖNG (nread == nwrite), thay vì FULL.
//    - Reader dùng copyOUT (kernel → user), writer dùng copyIN (user → kernel).
//    - Reader trả về NGAY khi không còn data, không chờ đủ n byte (semantic
//      của UNIX read).
//
// ----------------------------------------------------------------------------
//  TÓM TẮT - VÒNG ĐỜI 1 CHUNK:
// ----------------------------------------------------------------------------
//      User write()                Kernel pipewrite()           Pipe buffer
//          │                              │                          │
//          │  ecall ───────────────────►  │                          │
//          │                              │  acquire lock            │
//          │                              │  copy chunk →            │ +chunk
//          │                              │  (đè vào nwrite slot)    │
//          │                              │  nwrite += chunk         │
//          │                              │  wakeup reader           │
//          │                              │  release lock            │
//          │  ◄───────────── trapret ──── │                          │
//          ▼                              ▼                          │
//      tiếp tục loop                                                 │
//                                                                    │
//      User read()                Kernel piperead()                  │
//          │                              │                          │
//          │  ecall ───────────────────►  │                          │
//          │                              │  acquire lock            │
//          │                              │  copy chunk ←            │ -chunk
//          │                              │  (lấy từ nread slot)     │
//          │                              │  nread += chunk          │
//          │                              │  wakeup writer           │
//          │                              │  release lock            │
//          │  ◄───────────── trapret ──── │                          │
//          ▼                                                         │
//      tiếp tục loop                                                 │
//
//  Sau N lần lặp ở cả 2 phía, total_bytes đã được truyền qua pipe.
// ----------------------------------------------------------------------------
//
//  CÁCH DÙNG:
//    pipebench                     # quét chunk_bytes (mặc định total = 4MB)
//    pipebench chunk [total_bytes] # quét chunk_bytes với total tự chọn
//    pipebench total [chunk_bytes] # quét total_bytes với chunk tự chọn
//
//  OUTPUT:
//    1. In bảng kết quả ra console (để xem trực tiếp)
//    2. Ghi file ipctp.txt trong filesystem xv6 (để host parse và vẽ chart)
// ============================================================================

#include "kernel/types.h"
#include "kernel/fcntl.h"   // O_CREATE, O_WRONLY, O_TRUNC cho open()
#include "user/user.h"      // pipe(), fork(), read(), write(), uptime()...

// ----- Hằng số cấu hình ------------------------------------------------------
// xv6 đếm thời gian theo tick. Trong kernel/proc.c (timervec) tick được tăng
// mỗi lần timer interrupt. Trên QEMU virt board, mặc định 1 tick ~ 100ms,
// tức 10 tick/giây. Hằng số này dùng để quy đổi tick → giây.
#define TICKS_PER_SEC      10

// Tổng số byte mặc định sẽ truyền qua pipe khi đo. 4MB đủ lớn để elapsed
// ticks ≥ 1 (tránh chia cho 0), đủ nhỏ để chạy nhanh trong QEMU.
#define DEFAULT_TOTAL      (4 * 1024 * 1024)

// Kích thước mỗi lần read/write mặc định khi không quét chunk_bytes.
// 512 byte là kích thước "vừa phải" - nhỏ hơn PIPESIZE (~4096) nhưng đủ
// lớn để không bị overhead system call quá nhiều.
#define DEFAULT_CHUNK      512

// Tên file output. File này được tạo TRONG filesystem của xv6 (fs.img),
// không phải trên Linux host. Phải dùng `cat ipctp.txt` trong xv6 để xem.
#define OUTPUT_FILE        "ipctp.txt"

// ----- Mảng giá trị quét -----------------------------------------------------
// Quét theo chuỗi lũy thừa của 2: mỗi bước tăng gấp đôi.
// Lý do: thông lượng phụ thuộc gần như tuyến tính theo log2(chunk_bytes)
// vì mỗi syscall đều có overhead cố định (mode-switch, lock acquire/release).
// Khi chunk nhỏ, overhead chiếm tỉ lệ lớn ⇒ throughput thấp.
// Khi chunk lớn, overhead nhỏ tương đối ⇒ throughput tăng và sẽ bão hòa
// gần PIPESIZE (4KB) vì lúc đó mỗi write lấp đầy buffer.
static int chunk_values[] = {64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};

// Quét theo total_bytes để kiểm tra xem throughput có ổn định khi truyền
// nhiều dữ liệu hơn không (loại trừ ảnh hưởng warm-up của lần đo đầu).
static int total_values[] = {
  256 * 1024,         // 256 KB - có thể fit trong vài lần fill buffer
  512 * 1024,         // 512 KB
  1 * 1024 * 1024,    // 1 MB
  2 * 1024 * 1024,    // 2 MB
  4 * 1024 * 1024,    // 4 MB - giá trị mặc định
  8 * 1024 * 1024,    // 8 MB - đủ lớn để thấy steady-state throughput
};

// Hàm tiện ích lấy min của 2 số nguyên. Tự viết vì xv6 ulib không có.
static int
imin(int a, int b)
{
  return a < b ? a : b;
}

// ============================================================================
//  bench() - chạy 1 phép đo throughput cho cặp (total_bytes, chunk_bytes)
// ============================================================================
//  Cách hoạt động (mô hình producer-consumer qua pipe):
//
//    parent (writer)                          child (reader)
//        |                                          |
//        | -- pipe() tạo p[0] đọc, p[1] ghi -->     |
//        | -- fork() ----------------------------> |
//        | close(p[0]) (chỉ ghi)                   close(p[1]) (chỉ đọc)
//        | start = uptime()                         |
//        | while(còn dữ liệu)                       while(chưa nhận đủ)
//        |   write(p[1], buf, chunk)                  read(p[0], buf, chunk)
//        | close(p[1]) (báo EOF)                     |
//        | wait(child)                               exit(0)
//        | end = uptime()                            |
//        | return throughput
//
//  TẠI SAO CẦN HAI PROCESS?
//    Pipe trong xv6 dùng spinlock + sleep/wakeup để đồng bộ. Nếu chỉ
//    1 process vừa write vừa read thì khi buffer đầy, write sẽ sleep
//    mà không có ai wake-up ⇒ deadlock. Hai process độc lập đảm bảo
//    bên đọc luôn tiêu thụ song song với bên ghi.
//
//  ĐO Ở ĐÂU?
//    Đo từ lúc bắt đầu write đến khi child exit (parent wait xong).
//    Thời gian này bao gồm:
//      - copyin user→kernel của writer
//      - sleep/wakeup khi buffer full/empty
//      - context switch giữa writer và reader
//      - copyout kernel→user của reader
//    ⇒ Phản ánh đúng cost của TOÀN BỘ pipeline IPC.
//
//  TRẢ VỀ:
//    Throughput tính theo Byte/s, hoặc -1 nếu lỗi (malloc/pipe/fork fail).
// ============================================================================
static int
bench(int total_bytes, int chunk_bytes)
{
  int p[2];                  // p[0]: read end, p[1]: write end
  int pid;
  char *buf;
  int sent = 0;
  int start_ticks, end_ticks, elapsed;

  // Cấp phát buffer trên heap (không stack vì chunk có thể tới 4KB - vượt
  // stack mặc định của xv6 user process).
  buf = malloc(chunk_bytes);
  if(buf == 0)
    return -1;
  // Điền dữ liệu giả "aaaa..." để có nội dung gì đó truyền đi. Nội dung
  // không quan trọng, chỉ cần kích thước đủ.
  memset(buf, 'a', chunk_bytes);

  // pipe() syscall gọi kernel/sysfile.c::sys_pipe → kernel/pipe.c::pipealloc.
  // Sau khi return: p[0] và p[1] là 2 file descriptor trỏ vào cùng struct pipe.
  if(pipe(p) < 0){
    free(buf);
    return -1;
  }

  // fork() tạo child process là bản sao của parent. Child kế thừa toàn bộ
  // file descriptor table → cả p[0], p[1] đều mở ở cả 2 process.
  pid = fork();
  if(pid < 0){
    close(p[0]);
    close(p[1]);
    free(buf);
    return -1;
  }

  // ==== CHILD = READER (consumer) =========================================
  if(pid == 0){
    int got = 0, n;
    // Child KHÔNG ghi nên phải đóng đầu ghi. Quan trọng: nếu không đóng,
    // pipe sẽ không bao giờ "writable == 0" nên reader sẽ không nhận được
    // EOF khi parent đóng đầu ghi của mình.
    close(p[1]);
    while(got < total_bytes){
      // Đọc tối đa chunk_bytes mỗi lần. min() đảm bảo lần đọc cuối không
      // vượt quá tổng cần nhận (tránh chờ thêm dữ liệu không có).
      n = read(p[0], buf, imin(chunk_bytes, total_bytes - got));
      if(n <= 0)   // 0 = EOF (writer đóng), <0 = lỗi
        break;
      got += n;
    }
    close(p[0]);
    exit(0);
  }

  // ==== PARENT = WRITER (producer) ========================================
  // Parent KHÔNG đọc nên đóng đầu đọc - cần thiết để khi parent close(p[1]),
  // pipe biết không còn writer nào và child sẽ nhận EOF.
  close(p[0]);

  // BẮT ĐẦU ĐO THỜI GIAN.
  // uptime() syscall trả về số ticks từ khi xv6 boot (kernel/sysproc.c::sys_uptime).
  start_ticks = uptime();

  while(sent < total_bytes){
    int n = write(p[1], buf, imin(chunk_bytes, total_bytes - sent));
    if(n <= 0){
      // Lỗi nghiêm trọng (reader chết, system out of memory...). Phải clean
      // up đầy đủ để không leak resource: đóng pipe, đợi child, free buf.
      close(p[1]);
      wait(0);
      free(buf);
      return -1;
    }
    sent += n;
  }
  // Đóng đầu ghi → child sẽ thấy read() return 0 (EOF) và thoát vòng lặp.
  close(p[1]);
  // Đợi child exit để đảm bảo TẤT CẢ dữ liệu đã được tiêu thụ xong trước
  // khi đo end_ticks. Nếu không wait, parent sẽ "đo" thiếu phần thời gian
  // child còn đang đọc nốt buffer.
  wait(0);

  // KẾT THÚC ĐO.
  end_ticks = uptime();

  elapsed = end_ticks - start_ticks;
  // Phép phòng thủ: tránh chia cho 0 khi total_bytes nhỏ và elapsed = 0.
  // Lúc đó thông lượng được "underestimated" nhưng không crash.
  if(elapsed <= 0)
    elapsed = 1;
  free(buf);

  // Throughput (B/s) = total_bytes / (elapsed_ticks / TICKS_PER_SEC)
  //                  = total_bytes * TICKS_PER_SEC / elapsed_ticks
  // Dùng long long để tránh overflow khi total_bytes lớn (> ~200MB với int32).
  return (int)(((long long)total_bytes * TICKS_PER_SEC) / elapsed);
}

// ============================================================================
//  itoa() - chuyển số nguyên thành chuỗi
// ============================================================================
// xv6 ulib không có sprintf/snprintf nên phải tự viết để format số khi
// ghi vào file (write() chỉ nhận buffer raw). Algorithm: lấy mod 10 ngược
// rồi đảo chuỗi.
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
  // Đảo ngược tmp[] vào buf[] để được thứ tự đúng (số có nghĩa nhất ở đầu).
  for(j = 0; j < i; j++)
    buf[j] = tmp[i - 1 - j];
  buf[i] = 0;
}

// Ghi 1 chuỗi vào fd (gọi strlen + write).
static void
write_str(int fd, char *s)
{
  write(fd, s, strlen(s));
}

// Ghi 1 dòng "<label> <value>\n" vào file output.
// Format này được parse bởi tools/plot_throughput.py và
// tools/compare_throughput.py trên host.
static void
write_record(int fd, int label, int value)
{
  char nl[16], nv[16];
  itoa(label, nl);
  itoa(value, nv);
  write_str(fd, nl);
  write_str(fd, " ");
  write_str(fd, nv);
  write_str(fd, "\n");
}

// ============================================================================
//  sweep_chunk() - quét qua các chunk_bytes, giữ nguyên total_bytes
// ============================================================================
// Đây là chế độ chạy MẶC ĐỊNH. Mục đích: thấy được thông lượng thay đổi
// thế nào theo kích thước mỗi lần read/write. Đây là phép đo quan trọng
// nhất cho việc đánh giá bulk-copy optimization vì:
//   - chunk nhỏ ⇒ nhiều syscall ⇒ overhead lớn ⇒ thấy rõ lợi ích bulk-copy
//   - chunk lớn ⇒ ít syscall ⇒ optimization gần như không khác baseline
static int
sweep_chunk(int total_bytes, int fd)
{
  int n_values = sizeof(chunk_values) / sizeof(chunk_values[0]);
  int i;

  printf("==== IPC throughput sweep: vary chunk_bytes ====\n");
  printf("  total_bytes = %d\n", total_bytes);
  printf("  chunk_bytes  |  B/s\n");
  printf("  -------------|-------------------\n");

  write_str(fd, "# xaxis=chunk_bytes total_bytes=");
  {
    char ntb[16];
    itoa(total_bytes, ntb);
    write_str(fd, ntb);
    write_str(fd, "\n");
  }

  for(i = 0; i < n_values; i++){
    int bps = bench(total_bytes, chunk_values[i]);
    if(bps < 0){
      printf("  %d  |  FAILED\n", chunk_values[i]);
      continue;
    }
    printf("  %d  |  %d\n", chunk_values[i], bps);
    write_record(fd, chunk_values[i], bps);
  }
  return 0;
}

// ============================================================================
//  sweep_total() - quét qua các total_bytes, giữ nguyên chunk_bytes
// ============================================================================
// Chế độ phụ. Dùng để kiểm tra xem throughput có ổn định khi tăng tổng
// dữ liệu không (loại trừ ảnh hưởng cache warm-up, jitter của lần đo
// đầu). Nếu kết quả gần-flat khi total tăng, nghĩa là phép đo đáng tin.
static int
sweep_total(int chunk_bytes, int fd)
{
  int n_values = sizeof(total_values) / sizeof(total_values[0]);
  int i;

  printf("==== IPC throughput sweep: vary total_bytes ====\n");
  printf("  chunk_bytes = %d\n", chunk_bytes);
  printf("  total_bytes  |  B/s\n");
  printf("  -------------|-------------------\n");

  write_str(fd, "# xaxis=total_bytes chunk_bytes=");
  {
    char nch[16];
    itoa(chunk_bytes, nch);
    write_str(fd, nch);
    write_str(fd, "\n");
  }

  for(i = 0; i < n_values; i++){
    int bps = bench(total_values[i], chunk_bytes);
    if(bps < 0){
      printf("  %d  |  FAILED\n", total_values[i]);
      continue;
    }
    printf("  %d  |  %d\n", total_values[i], bps);
    write_record(fd, total_values[i], bps);
  }
  return 0;
}

// ============================================================================
//  main() - phân tích argv, mở file output, dispatch sang sweep tương ứng
// ============================================================================
int
main(int argc, char *argv[])
{
  int mode = 0;                       // 0: sweep chunk, 1: sweep total, 2: once
  int total_bytes = DEFAULT_TOTAL;
  int chunk_bytes = DEFAULT_CHUNK;
  int fd;

  // ---- Parse argv ----------------------------------------------------------
  // argv[1]: chế độ ("chunk", "total", hoặc "once")
  if(argc >= 2){
    if(strcmp(argv[1], "chunk") == 0)
      mode = 0;
    else if(strcmp(argv[1], "total") == 0)
      mode = 1;
    else if(strcmp(argv[1], "once") == 0)
      mode = 2;
    else {
      fprintf(2, "usage: pipebench [chunk|total|once] [param1] [param2]\n");
      fprintf(2, "  pipebench                        -- sweep chunk, total=default\n");
      fprintf(2, "  pipebench chunk [total_bytes]    -- sweep chunk, fix total\n");
      fprintf(2, "  pipebench total [chunk_bytes]    -- sweep total, fix chunk\n");
      fprintf(2, "  pipebench once [total] [chunk]   -- do lap, total va chunk bat ki\n");
      exit(1);
    }
  }

  if(mode == 2){
    // once: pipebench once [total_bytes] [chunk_bytes]
    if(argc >= 3){
      int v = atoi(argv[2]);
      if(v > 0) total_bytes = v;
    }
    if(argc >= 4){
      int v = atoi(argv[3]);
      if(v > 0) chunk_bytes = v;
    }
    int bps = bench(total_bytes, chunk_bytes);
    if(bps < 0){
      fprintf(2, "pipebench once: FAILED\n");
      exit(1);
    }
    printf("total=%d chunk=%d => %d B/s\n", total_bytes, chunk_bytes, bps);
    exit(0);
  }

  // argv[2]: giá trị tham số đi kèm. Tùy mode mà nó là total hay chunk.
  if(argc >= 3){
    int v = atoi(argv[2]);
    if(v <= 0){
      fprintf(2, "pipebench: bad numeric arg\n");
      exit(1);
    }
    if(mode == 0)
      total_bytes = v;     // mode chunk: cố định total, quét chunk
    else
      chunk_bytes = v;     // mode total: cố định chunk, quét total
  }

  // ---- Mở file output ------------------------------------------------------
  fd = open(OUTPUT_FILE, O_CREATE | O_WRONLY | O_TRUNC);
  if(fd < 0){
    fprintf(2, "pipebench: cannot open %s for write\n", OUTPUT_FILE);
    exit(1);
  }

  // ---- Chạy sweep ----------------------------------------------------------
  if(mode == 0)
    sweep_chunk(total_bytes, fd);
  else
    sweep_total(chunk_bytes, fd);

  close(fd);
  printf("Saved results to %s\n", OUTPUT_FILE);
  printf("Run 'plotchart' to draw the chart.\n");
  exit(0);
}
