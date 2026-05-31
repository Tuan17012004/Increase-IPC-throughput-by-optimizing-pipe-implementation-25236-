#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"

// Saved registers for kernel context switches.
struct context {
  uint64 ra;
  uint64 sp;

  // callee-saved
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// Per-CPU state.
struct cpu {
  struct proc *proc;          // The process running on this cpu, or null.
  struct context context;     // swtch() here to enter scheduler().
  int noff;                   // Depth of push_off() nesting.
  int intena;                 // Were interrupts enabled before push_off()?
};

extern struct cpu cpus[NCPU];

// per-process data for the trap handling code in trampoline.S.
// sits in a page by itself just under the trampoline page in the
// user page table. not specially mapped in the kernel page table.
// uservec in trampoline.S saves user registers in the trapframe,
// then initializes registers from the trapframe's
// kernel_sp, kernel_hartid, kernel_satp, and jumps to kernel_trap.
// usertrapret() and userret in trampoline.S set up
// the trapframe's kernel_*, restore user registers from the
// trapframe, switch to the user page table, and enter user space.
// the trapframe includes callee-saved user registers like s0-s11 because the
// return-to-user path via usertrapret() doesn't return through
// the entire kernel call stack.
struct trapframe {
  /*   0 */ uint64 kernel_satp;   // kernel page table
  /*   8 */ uint64 kernel_sp;     // top of process's kernel stack
  /*  16 */ uint64 kernel_trap;   // usertrap()
  /*  24 */ uint64 epc;           // saved user program counter
  /*  32 */ uint64 kernel_hartid; // saved kernel tp
  /*  40 */ uint64 ra;
  /*  48 */ uint64 sp;
  /*  56 */ uint64 gp;
  /*  64 */ uint64 tp;
  /*  72 */ uint64 t0;
  /*  80 */ uint64 t1;
  /*  88 */ uint64 t2;
  /*  96 */ uint64 s0;
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// ============================================================================
//  Cơ chế 11: Priority Boost — Hằng số ưu tiên
// ============================================================================
//
//  VẤN ĐỀ (xv6 gốc — Round-Robin thuần):
//    Scheduler xv6 quét tuần tự proc[0], proc[1], ..., proc[63] và chọn
//    process RUNNABLE ĐẦU TIÊN tìm thấy. Không phân biệt process "quan
//    trọng" hay "nhàn rỗi".
//
//    Hệ quả cho pipe IPC:
//      t=0:  Writer gọi wakeup(reader) — reader chuyển SLEEPING → RUNNABLE
//      t=1:  Scheduler quét proc[] — nhưng reader có thể ở cuối bảng!
//            → Scheduler chọn process A, B, C khác trước
//      t=N:  Sau nhiều tick, scheduler mới chọn reader
//            → Writer đã sleep vì buffer đầy, nhưng reader chưa chạy
//            → Pipeline bị IDLE trong (N-1) tick × 100ms = hàng trăm ms!
//
//    Kịch bản steady-state (round-robin):
//      Writer ghi → buffer đầy → wakeup(reader) → sleep(writer)
//           ↓
//      Scheduler: proc[0]? proc[1]? ... proc[K]=reader? → chạy reader
//           ↓
//      Reader đọc → buffer rỗng → wakeup(writer) → sleep(reader)
//           ↓
//      Scheduler: proc[0]? ... proc[J]=writer? → chạy writer
//
//      Nếu có nhiều process khác (init, sh, ...), mỗi lần chuyển writer↔reader
//      phải chờ scheduler quét qua các process đó → latency tăng.
//
//  GIẢI PHÁP — Priority Boost:
//    Thêm trường `priority` vào struct proc. Khi wakeup() đánh thức một
//    process (đặc biệt trong pipe), đồng thời TĂNG priority lên cao
//    (PRIORITY_BOOST). Scheduler chọn process có priority CAO NHẤT thay
//    vì chọn theo thứ tự bảng.
//
//    Kết quả:
//      t=0:  Writer: wakeup(reader) → reader.priority = PRIORITY_BOOST
//      t=1:  Scheduler: quét proc[] → reader có priority cao nhất → CHỌN NGAY!
//      t=2:  Reader đọc → wakeup(writer) → writer.priority = PRIORITY_BOOST
//      t=3:  Scheduler: chọn writer ngay!
//      → Pipeline chạy liên tục, ít idle time!
//
//  DECAY MECHANISM:
//    Sau mỗi timer tick, nếu process đang chạy có priority > BASE thì
//    giảm priority đi 1. Điều này đảm bảo:
//      a) Process không chiếm CPU vĩnh viễn (tránh starvation)
//      b) Priority boost chỉ có tác dụng ngắn hạn (~10 tick = ~1 giây)
//      c) Các process thường dần trở về priority bình thường
//
//    Ví dụ decay:
//      tick 0:  reader wakeup → priority = 10 (BOOST)
//      tick 1:  reader chạy   → priority = 9  (decay)
//      tick 2:  reader chạy   → priority = 8
//      ...
//      tick 10: reader         → priority = 0  (BASE, về bình thường)
//
// ============================================================================
#define PRIORITY_BASE   0    // Priority mặc định cho mọi process
#define PRIORITY_BOOST  10   // Priority cao khi được wakeup từ pipe

// Cơ chế 11 (priority boost) chỉ kích hoạt khi PIPE_VERSION >= 6.
// Với v1..v5: scheduler là round-robin gốc, wakeup không boost.
#ifndef PIPE_VERSION
#define PIPE_VERSION 7
#endif

// Per-process state
struct proc {
  struct spinlock lock;

  // p->lock must be held when using these:
  enum procstate state;        // Process state
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  int xstate;                  // Exit status to be returned to parent's wait
  int pid;                     // Process ID

  // wait_lock must be held when using this:
  struct proc *parent;         // Parent process

  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  uint64 sz;                   // Size of process memory (bytes)
  pagetable_t pagetable;       // User page table
  struct trapframe *trapframe; // data page for trampoline.S
  struct context context;      // swtch() here to run process
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)

  // ---- Cơ chế 11: Priority Boost ----
  //
  // Giá trị ưu tiên của process. Scheduler sẽ chọn process có priority
  // cao nhất trong tất cả RUNNABLE process.
  //
  //   PRIORITY_BASE  (0)  = bình thường (giống round-robin cũ)
  //   PRIORITY_BOOST (10) = vừa được wakeup từ pipe → chạy ngay
  //
  // Decay: mỗi timer tick, nếu priority > BASE thì priority--.
  // Đảm bảo process không chiếm CPU vĩnh viễn.
  int priority;
};
