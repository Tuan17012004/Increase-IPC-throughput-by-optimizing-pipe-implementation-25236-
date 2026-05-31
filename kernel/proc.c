#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // ---- Cơ chế 11: khởi tạo priority = BASE ----
  // Trường priority luôn tồn tại trong struct proc, nhưng chỉ ĐƯỢC DÙNG
  // bởi scheduler/wakeup khi PIPE_VERSION >= 6.
  p->priority = PRIORITY_BASE;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->priority = PRIORITY_BASE;  // Cơ chế 11: reset priority khi free
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if(sz + n > TRAPFRAME) {
      return -1;
    }
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// ============================================================================
//  scheduler() — Cơ chế 11: Priority-Based Scheduling
// ============================================================================
//
//  TRƯỚC (xv6 gốc — Round-Robin):
//    Scheduler quét tuần tự proc[0..63], chọn process RUNNABLE ĐẦU TIÊN:
//
//      for(p = proc; p < &proc[NPROC]; p++) {
//        if(p->state == RUNNABLE) {
//          p->state = RUNNING;   // ← chạy luôn, không so sánh
//          swtch(...);
//        }
//      }
//
//    Vấn đề: nếu reader pipe ở proc[60] và process shell ở proc[2],
//    shell sẽ LUÔN được chạy trước reader → reader bị trì hoãn
//    → writer sleep lâu → throughput giảm.
//
//  SAU (v6 — Priority Boost):
//    Scheduler quét TOÀN BỘ proc[], tìm process RUNNABLE có priority
//    CAO NHẤT, rồi mới chạy process đó.
//
//    THUẬT TOÁN:
//      1. best = NULL, best_priority = -1
//      2. for(p in proc[0..63]):
//           if p.state == RUNNABLE && p.priority > best_priority:
//             best = p
//             best_priority = p.priority
//      3. if best != NULL: swtch(best)
//
//    Nếu nhiều process có CÙNG priority (ví dụ tất cả = 0):
//      → chọn process ĐẦU TIÊN tìm thấy (giống round-robin cũ)
//      → backward-compatible!
//
//  CHI PHÍ:
//    Round-robin: tìm thấy 1 RUNNABLE → dừng ngay (tối ưu nếu ít process)
//    Priority:    LUÔN quét hết 64 entry để tìm max
//    → Thêm ~64 lần kiểm tra mỗi vòng scheduler.
//    Nhưng: scheduler loop chạy trong kernel, 64 compare rất nhanh (~vài μs).
//    Lợi ích: giảm latency pipe IPC từ O(NPROC ticks) xuống O(1 tick).
//
// ============================================================================
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // Enable interrupts briefly to avoid deadlock when all processes sleep.
    intr_on();
    intr_off();

#if PIPE_VERSION >= 6
    // ---- PHASE 1: Tìm process RUNNABLE có priority cao nhất ----
    //
    // Quét toàn bộ proc[]. Với mỗi RUNNABLE process, so sánh priority.
    // Giữ lại process có priority cao nhất.
    //
    // LƯU Ý LOCK:
    //   Chúng ta KHÔNG giữ lock khi quét tìm best (chỉ đọc state + priority).
    //   Điều này an toàn vì:
    //     a) state và priority chỉ thay đổi dưới p->lock
    //     b) Nếu process thay đổi state sau khi ta đọc → ta sẽ thử acquire
    //        lock và kiểm tra lại ở phase 2 (double-check)
    //     c) Worst case: miss một RUNNABLE process → sẽ bắt ở vòng sau
    struct proc *best = 0;
    int best_priority = -1;

    for(p = proc; p < &proc[NPROC]; p++) {
      // Đọc state KHÔNG cần lock (optimization: tránh 64 acquire/release).
      // Có thể đọc stale value nhưng vô hại (xem lưu ý ở trên).
      if(p->state == RUNNABLE && p->priority > best_priority) {
        best = p;
        best_priority = p->priority;
      }
    }

    // ---- PHASE 2: Chạy process đã chọn (double-check dưới lock) ----
    //
    // Acquire lock và KIỂM TRA LẠI state vì có thể process đã bị
    // thay đổi giữa phase 1 và phase 2 (race condition an toàn).
    if(best) {
      acquire(&best->lock);
      if(best->state == RUNNABLE) {
        best->state = RUNNING;
        c->proc = best;
        swtch(&c->context, &best->context);

        // Process đã yield/sleep/exit — trở về scheduler.
        c->proc = 0;
      }
      release(&best->lock);
    } else {
      // Không có process nào RUNNABLE → chờ interrupt.
      asm volatile("wfi");
    }
#else
    // ---- ROUND-ROBIN gốc của xv6 (cho v1..v5) ----
    // Quét tuần tự, chọn RUNNABLE đầu tiên tìm thấy. Không dùng priority.
    int found = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if(found == 0) {
      asm volatile("wfi");
    }
#endif
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// ============================================================================
//  yield() — Nhường CPU + Priority Decay (Cơ chế 11)
// ============================================================================
//
//  TRƯỚC (xv6 gốc):
//    Chỉ đặt state = RUNNABLE rồi sched().
//
//  SAU (v6 — Priority Boost):
//    Trước khi yield, GIẢM priority đi 1 nếu priority > PRIORITY_BASE.
//
//  TẠI SAO DECAY Ở ĐÂY?
//    yield() được gọi bởi timer interrupt handler (trap.c, dòng 85):
//      if(which_dev == 2)   // timer interrupt
//        yield();
//
//    Timer interrupt xảy ra mỗi tick (~100ms). Vậy mỗi tick, process
//    đang chạy sẽ bị giảm priority 1 đơn vị.
//
//    Ví dụ (reader vừa được wakeup với priority = PRIORITY_BOOST = 10):
//      tick 0: reader chạy, yield() → priority 10 → 9
//      tick 1: reader chạy, yield() → priority 9 → 8
//      ...
//      tick 10: reader chạy, yield() → priority 1 → 0 (BASE)
//      tick 11+: reader chạy, priority = 0 (KHÔNG giảm thêm)
//
//    → Priority boost chỉ hiệu quả trong ~10 tick (~1 giây).
//    → Sau đó process trở về priority bình thường,
//       scheduler lại hoạt động gần giống round-robin.
//    → TRÁNH STARVATION: process có priority boost không thể
//       chiếm CPU vĩnh viễn vì priority tự giảm dần.
//
// ============================================================================
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);

#if PIPE_VERSION >= 6
  // ---- Cơ chế 11: Priority Decay ----
  // Giảm priority mỗi tick để process không chiếm CPU mãi.
  if(p->priority > PRIORITY_BASE)
    p->priority--;
#endif

  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// ============================================================================
//  wakeup() — Đánh thức + Priority Boost (Cơ chế 11)
// ============================================================================
//
//  TRƯỚC (xv6 gốc):
//    void wakeup(void *chan) {
//      for(p in proc[0..63]) {
//        acquire(&p->lock);
//        if(p->state == SLEEPING && p->chan == chan)
//          p->state = RUNNABLE;              // ← chỉ đổi state
//        release(&p->lock);
//      }
//    }
//    → Process tỉnh dậy nhưng priority = 0 (BASE)
//    → Scheduler có thể chọn process khác trước!
//
//  SAU (v6 — Priority Boost):
//    void wakeup(void *chan) {
//      ...
//      if(p->state == SLEEPING && p->chan == chan) {
//        p->state = RUNNABLE;
//        p->priority = PRIORITY_BOOST;       // ← BOOST priority!
//      }
//      ...
//    }
//    → Process tỉnh dậy với priority = 10 (BOOST)
//    → Scheduler sẽ chọn process này TRƯỚC process có priority thấp hơn!
//
//  TÁC DỤNG CHO PIPE IPC:
//    pipewrite() gọi wakeup(&pi->nread) khi ghi xong:
//      → reader.priority = PRIORITY_BOOST
//      → Scheduler chọn reader ngay tick kế tiếp
//      → Reader đọc xong, gọi wakeup(&pi->nwrite):
//          → writer.priority = PRIORITY_BOOST
//          → Scheduler chọn writer ngay tick kế tiếp
//      → Pipeline chạy liên tục, ping-pong tối ưu!
//
//  VÍ DỤ TRƯỚC/SAU:
//    Hệ thống có: [init(pri=0), sh(pri=0), reader(pri=0), writer(pri=0)]
//
//    TRƯỚC (round-robin):
//      Writer wakeup(reader): reader.state = RUNNABLE, priority = 0
//      Scheduler: init? → skip (sleeping)
//                 sh? → RUNNABLE, priority = 0 → CHỌN sh!
//                 reader phải chờ sh chạy xong tick
//
//    SAU (priority boost):
//      Writer wakeup(reader): reader.state = RUNNABLE, priority = 10
//      Scheduler: init? pri=0
//                 sh?   pri=0
//                 reader? pri=10 → CHỌN reader! (cao nhất)
//
// ============================================================================
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
#if PIPE_VERSION >= 6
        // ---- Cơ chế 11: BOOST priority (chỉ v6+) ----
        p->priority = PRIORITY_BOOST;
#endif
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]   = "unused",
  [USED]     = "used",
  [SLEEPING] = "sleep ",
  [RUNNABLE] = "runble",
  [RUNNING]  = "run   ",
  [ZOMBIE]   = "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
