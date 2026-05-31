// ============================================================================
//  kernel/pipe.c  —  Multi-version pipe for IPC throughput comparison
// ============================================================================
//
//  Build with:  make PIPE_VERSION=N  (N = 1..8)
//
//    v1: ORIGINAL XV6        — 512B embedded, byte-by-byte copy
//    v2: BULK COPY           — 512B embedded, copyin/copyout chunks
//    v3: RING OF BUFFERS     — 2048B embedded (4 × 512B), bulk
//    v4: MULTI-PAGE          — 1 page metadata + 1 page data (4096B), bulk
//    v5: LAZY WAKEUP         — v4 + skip wakeup() when peer not sleeping
//    v6: PRIORITY BOOST      — v5 + scheduler boost (see proc.c)
//    v7: CACHE-LINE ALIGNED  — v6 + struct fields on separate cache lines
//    v8: ALL COMBINED        — v2+v3+v4+v5+v6+v7 (bulk copy, 4KB page, lazy wakeup, priority, cache-align)
//
//  Each version is fully self-contained between #if/#endif blocks below.
//  Version N includes ALL improvements of versions <= N.
// ============================================================================

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

#ifndef PIPE_VERSION
#define PIPE_VERSION 7   // default = fully optimized
#endif

#ifndef PIPE_TRACE
#define PIPE_TRACE 0
#endif

#if PIPE_TRACE
// 'W' when inside pipewrite path, 'R' when inside piperead path, 0 otherwise.
// Read by prepare_return() in trap.c to print the final step of the trace.
int pipe_trace_active = 0;
#endif

// ============================================================================
//  STRUCT DEFINITIONS — version-dependent
// ============================================================================

#if PIPE_VERSION == 1 || PIPE_VERSION == 2
// v1, v2: original 512-byte embedded buffer
#define PIPESIZE 512
struct pipe {
  struct spinlock lock;
  char data[PIPESIZE];
  uint nread;
  uint nwrite;
  int  readopen;
  int  writeopen;
};
#define PIPE_TOTAL PIPESIZE

#elif PIPE_VERSION == 3
// v3: ring of 4 × 512B buffers (still embedded in 1 page)
#define N_BUFS    4
#define BUFSIZE   512
#define PIPE_TOTAL (N_BUFS * BUFSIZE)
struct pipe {
  struct spinlock lock;
  char data[N_BUFS][BUFSIZE];
  uint nread;
  uint nwrite;
  int  readopen;
  int  writeopen;
};

#elif PIPE_VERSION == 4 || PIPE_VERSION == 5 || PIPE_VERSION == 6
// v4..v6: 1 page metadata + 1 page data (4096B)
#define DATA_PAGES 1
#define PIPE_TOTAL (DATA_PAGES * PGSIZE)
struct pipe {
  struct spinlock lock;
  char *data;            // points to separate kalloc'd page
  uint nread;
  uint nwrite;
  int  readopen;
  int  writeopen;
};

#elif PIPE_VERSION == 7
// v7: same as v4-v6 but cache-line aligned to avoid false sharing
#define DATA_PAGES 1
#define PIPE_TOTAL (DATA_PAGES * PGSIZE)
#define CACHELINE  64
struct pipe {
  // Cache line 0: writer-side (lock + data ptr + nwrite)
  struct spinlock lock;
  char *data;
  uint nwrite;
  char _pad_w[CACHELINE - sizeof(struct spinlock) - sizeof(char*) - sizeof(uint)];
  // Cache line 1: reader-side (nread)
  uint nread;
  char _pad_r[CACHELINE - sizeof(uint) - 2 * sizeof(int)];
  // Cache line 2: rarely-changed flags
  int  readopen;
  int  writeopen;
};

#elif PIPE_VERSION == 8
// v8: ALL COMBINED — identical to v7 (bulk copy + 4KB page + lazy wakeup + priority boost + cache-line align)
#define DATA_PAGES 1
#define PIPE_TOTAL (DATA_PAGES * PGSIZE)
#define CACHELINE  64
struct pipe {
  // Cache line 0: writer-side (lock + data ptr + nwrite)
  struct spinlock lock;
  char *data;
  uint nwrite;
  char _pad_w[CACHELINE - sizeof(struct spinlock) - sizeof(char*) - sizeof(uint)];
  // Cache line 1: reader-side (nread)
  uint nread;
  char _pad_r[CACHELINE - sizeof(uint) - 2 * sizeof(int)];
  // Cache line 2: rarely-changed flags
  int  readopen;
  int  writeopen;
};

#else
#error "Unsupported PIPE_VERSION (must be 1..8)"
#endif

#if PIPE_VERSION >= 2
static uint umin(uint a, uint b) { return a < b ? a : b; }
#endif

// ============================================================================
//  pipealloc / pipeclose — version-dependent (v4+ allocates 2 pages)
// ============================================================================

int
pipealloc(struct file **f0, struct file **f1)
{
  struct pipe *pi;

  pi = 0;
  *f0 = *f1 = 0;
  if((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  if((pi = (struct pipe*)kalloc()) == 0)
    goto bad;

#if PIPE_VERSION >= 4
  // v4-v7: data lives in its own single page
  if((pi->data = (char*)kalloc()) == 0){
    kfree((char*)pi);
    pi = 0;
    goto bad;
  }
#endif

  pi->readopen  = 1;
  pi->writeopen = 1;
  pi->nwrite    = 0;
  pi->nread     = 0;
  initlock(&pi->lock, "pipe");
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = pi;
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = pi;
  return 0;

 bad:
  if(pi)
    kfree((char*)pi);
  if(*f0)
    fileclose(*f0);
  if(*f1)
    fileclose(*f1);
  return -1;
}

void
pipeclose(struct pipe *pi, int writable)
{
  acquire(&pi->lock);
  if(writable){
    pi->writeopen = 0;
    wakeup(&pi->nread);
  } else {
    pi->readopen = 0;
    wakeup(&pi->nwrite);
  }
  if(pi->readopen == 0 && pi->writeopen == 0){
    release(&pi->lock);
#if PIPE_VERSION >= 4
    kfree(pi->data);
#endif
    kfree((char*)pi);
  } else
    release(&pi->lock);
}

// ============================================================================
//  pipewrite — 4 different implementations, selected by PIPE_VERSION
// ============================================================================

#if PIPE_VERSION == 1
// ----------------------------------------------------------------------------
//  v1: ORIGINAL XV6 — copy 1 byte per iteration via fetchaddr/copyin(1)
// ----------------------------------------------------------------------------
int
pipewrite(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();
#if PIPE_TRACE
  int _ncopy = 0;
  pipe_trace_active = 'W';
  printf("[TRACE W6] pipewrite()   : enter n=%d  nwrite=%u  nread=%u\n",
         n, pi->nwrite, pi->nread);
#endif
  acquire(&pi->lock);
  while(i < n){
    if(pi->readopen == 0 || killed(pr)){
      release(&pi->lock);
      return -1;
    }
    if(pi->nwrite == pi->nread + PIPESIZE){
      wakeup(&pi->nread);
      sleep(&pi->nwrite, &pi->lock);
    } else {
      char ch;
      if(copyin(pr->pagetable, &ch, addr + i, 1) == -1)
        break;
#if PIPE_TRACE
      _ncopy++;
#endif
      pi->data[pi->nwrite++ % PIPESIZE] = ch;
      i++;
    }
  }
#if PIPE_TRACE
  printf("[TRACE   ]   copyin()    : %d call(s) x 1B, user[0x%lx+0..%d] -> data[]\n",
         _ncopy, addr, n-1);
  printf("[TRACE   ]   wakeup()    : &pi->nread (unconditional, reader may not be sleeping)\n");
#endif
  wakeup(&pi->nread);   // unconditional
#if PIPE_TRACE
  printf("[TRACE W6] pipewrite()   : exit  wrote=%d  nwrite=%u\n", i, pi->nwrite);
#endif
  release(&pi->lock);
  return i;
}

#elif PIPE_VERSION == 2
// ----------------------------------------------------------------------------
//  v2: BULK COPY — single copyin per chunk, but still 512B buffer
// ----------------------------------------------------------------------------
int
pipewrite(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();

  acquire(&pi->lock);
  while(i < n){
    if(pi->readopen == 0 || killed(pr)){
      release(&pi->lock);
      return -1;
    }
    if(pi->nwrite == pi->nread + PIPESIZE){
      wakeup(&pi->nread);
      sleep(&pi->nwrite, &pi->lock);
    } else {
      uint free_slots = PIPESIZE - (pi->nwrite - pi->nread);
      uint head_slot  = pi->nwrite % PIPESIZE;
      uint till_wrap  = PIPESIZE - head_slot;
      int to_copy = (int)umin((uint)(n - i), umin(free_slots, till_wrap));
      if(copyin(pr->pagetable, &pi->data[head_slot], addr + i, to_copy) == -1)
        break;
      pi->nwrite += to_copy;
      i          += to_copy;
    }
  }
  wakeup(&pi->nread);
  release(&pi->lock);
  return i;
}

#elif PIPE_VERSION == 3
// ----------------------------------------------------------------------------
//  v3: RING OF 4 × 512B BUFFERS — bulk copy with 2D indexing
// ----------------------------------------------------------------------------
int
pipewrite(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();

  acquire(&pi->lock);
  while(i < n){
    if(pi->readopen == 0 || killed(pr)){
      release(&pi->lock);
      return -1;
    }
    if(pi->nwrite == pi->nread + PIPE_TOTAL){
      wakeup(&pi->nread);
      sleep(&pi->nwrite, &pi->lock);
    } else {
      uint free_slots = PIPE_TOTAL - (pi->nwrite - pi->nread);
      uint pos        = pi->nwrite % PIPE_TOTAL;
      uint buf_idx    = pos / BUFSIZE;
      uint buf_off    = pos % BUFSIZE;
      uint till_buf_end = BUFSIZE - buf_off;       // can't cross sub-buffer
      int to_copy = (int)umin((uint)(n - i), umin(free_slots, till_buf_end));
      if(copyin(pr->pagetable, &pi->data[buf_idx][buf_off], addr + i, to_copy) == -1)
        break;
      pi->nwrite += to_copy;
      i          += to_copy;
    }
  }
  wakeup(&pi->nread);
  release(&pi->lock);
  return i;
}

#elif PIPE_VERSION == 4
// ----------------------------------------------------------------------------
//  v4: MULTI-PAGE FLAT BUFFER — 1 separate page (4096B), 1D circular
// ----------------------------------------------------------------------------
int
pipewrite(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();

  acquire(&pi->lock);
  while(i < n){
    if(pi->readopen == 0 || killed(pr)){
      release(&pi->lock);
      return -1;
    }
    if(pi->nwrite == pi->nread + PIPE_TOTAL){
      wakeup(&pi->nread);
      sleep(&pi->nwrite, &pi->lock);
    } else {
      uint free_slots = PIPE_TOTAL - (pi->nwrite - pi->nread);
      uint head_slot  = pi->nwrite % PIPE_TOTAL;
      uint till_wrap  = PIPE_TOTAL - head_slot;
      int to_copy = (int)umin((uint)(n - i), umin(free_slots, till_wrap));
      if(copyin(pr->pagetable, &pi->data[head_slot], addr + i, to_copy) == -1)
        break;
      pi->nwrite += to_copy;
      i          += to_copy;
    }
  }
  wakeup(&pi->nread);
  release(&pi->lock);
  return i;
}

#else  // PIPE_VERSION >= 5 (includes v8)
// ----------------------------------------------------------------------------
//  v5/v6/v7/v8: LAZY WAKEUP — only call wakeup() when reader might be sleeping
// ----------------------------------------------------------------------------
int
pipewrite(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();

  acquire(&pi->lock);
  int need_wakeup = (pi->nwrite == pi->nread);  // buffer empty? reader may sleep

  while(i < n){
    if(pi->readopen == 0 || killed(pr)){
      release(&pi->lock);
      return -1;
    }
    if(pi->nwrite == pi->nread + PIPE_TOTAL){
      wakeup(&pi->nread);                 // mandatory before sleeping
      sleep(&pi->nwrite, &pi->lock);
      need_wakeup = 1;                    // after sleep, must re-notify
    } else {
      uint free_slots = PIPE_TOTAL - (pi->nwrite - pi->nread);
      uint head_slot  = pi->nwrite % PIPE_TOTAL;
      uint till_wrap  = PIPE_TOTAL - head_slot;
      int to_copy = (int)umin((uint)(n - i), umin(free_slots, till_wrap));
      if(copyin(pr->pagetable, &pi->data[head_slot], addr + i, to_copy) == -1)
        break;
      pi->nwrite += to_copy;
      i          += to_copy;
    }
  }
  if(need_wakeup && i > 0)
    wakeup(&pi->nread);
  release(&pi->lock);
  return i;
}
#endif

// ============================================================================
//  piperead — 4 different implementations, selected by PIPE_VERSION
// ============================================================================

#if PIPE_VERSION == 1
// v1: ORIGINAL — byte-by-byte
int
piperead(struct pipe *pi, uint64 addr, int n)
{
  int i;
  struct proc *pr = myproc();
  char ch;
#if PIPE_TRACE
  int _ncopy = 0;
  pipe_trace_active = 'R';
  printf("[TRACE R6] piperead()    : enter n=%d  nwrite=%u  nread=%u\n",
         n, pi->nwrite, pi->nread);
#endif
  acquire(&pi->lock);
  while(pi->nread == pi->nwrite && pi->writeopen){
    if(killed(pr)){
      release(&pi->lock);
      return -1;
    }
    sleep(&pi->nread, &pi->lock);
  }
  for(i = 0; i < n; i++){
    if(pi->nread == pi->nwrite)
      break;
    ch = pi->data[pi->nread++ % PIPESIZE];
    if(copyout(pr->pagetable, addr + i, &ch, 1) == -1)
      break;
#if PIPE_TRACE
    _ncopy++;
#endif
  }
#if PIPE_TRACE
  printf("[TRACE   ]   copyout()   : %d call(s) x 1B, data[] -> user[0x%lx+0..%d]\n",
         _ncopy, addr, n-1);
  printf("[TRACE   ]   wakeup()    : &pi->nwrite\n");
#endif
  wakeup(&pi->nwrite);
#if PIPE_TRACE
  printf("[TRACE R6] piperead()    : exit  read=%d  nread=%u\n", i, pi->nread);
#endif
  release(&pi->lock);
  return i;
}

#elif PIPE_VERSION == 2
// v2: bulk copy, 512B
int
piperead(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();

  acquire(&pi->lock);
  while(pi->nread == pi->nwrite && pi->writeopen){
    if(killed(pr)){
      release(&pi->lock);
      return -1;
    }
    sleep(&pi->nread, &pi->lock);
  }
  while(i < n){
    if(pi->nread == pi->nwrite)
      break;
    uint avail     = pi->nwrite - pi->nread;
    uint tail_slot = pi->nread % PIPESIZE;
    uint till_wrap = PIPESIZE - tail_slot;
    int to_copy = (int)umin((uint)(n - i), umin(avail, till_wrap));
    if(copyout(pr->pagetable, addr + i, &pi->data[tail_slot], to_copy) == -1){
      if(i == 0) i = -1;
      break;
    }
    pi->nread += to_copy;
    i         += to_copy;
  }
  wakeup(&pi->nwrite);
  release(&pi->lock);
  return i;
}

#elif PIPE_VERSION == 3
// v3: bulk copy, 2D ring of 4 × 512B
int
piperead(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();

  acquire(&pi->lock);
  while(pi->nread == pi->nwrite && pi->writeopen){
    if(killed(pr)){
      release(&pi->lock);
      return -1;
    }
    sleep(&pi->nread, &pi->lock);
  }
  while(i < n){
    if(pi->nread == pi->nwrite)
      break;
    uint avail        = pi->nwrite - pi->nread;
    uint pos          = pi->nread % PIPE_TOTAL;
    uint buf_idx      = pos / BUFSIZE;
    uint buf_off      = pos % BUFSIZE;
    uint till_buf_end = BUFSIZE - buf_off;
    int to_copy = (int)umin((uint)(n - i), umin(avail, till_buf_end));
    if(copyout(pr->pagetable, addr + i, &pi->data[buf_idx][buf_off], to_copy) == -1){
      if(i == 0) i = -1;
      break;
    }
    pi->nread += to_copy;
    i         += to_copy;
  }
  wakeup(&pi->nwrite);
  release(&pi->lock);
  return i;
}

#elif PIPE_VERSION == 4
// v4: bulk copy, 1D flat 4096B
int
piperead(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();

  acquire(&pi->lock);
  while(pi->nread == pi->nwrite && pi->writeopen){
    if(killed(pr)){
      release(&pi->lock);
      return -1;
    }
    sleep(&pi->nread, &pi->lock);
  }
  while(i < n){
    if(pi->nread == pi->nwrite)
      break;
    uint avail     = pi->nwrite - pi->nread;
    uint tail_slot = pi->nread % PIPE_TOTAL;
    uint till_wrap = PIPE_TOTAL - tail_slot;
    int to_copy = (int)umin((uint)(n - i), umin(avail, till_wrap));
    if(copyout(pr->pagetable, addr + i, &pi->data[tail_slot], to_copy) == -1){
      if(i == 0) i = -1;
      break;
    }
    pi->nread += to_copy;
    i         += to_copy;
  }
  wakeup(&pi->nwrite);
  release(&pi->lock);
  return i;
}

#else  // PIPE_VERSION >= 5 (includes v8)
// v5/v6/v7/v8: LAZY WAKEUP
int
piperead(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();

  acquire(&pi->lock);
  while(pi->nread == pi->nwrite && pi->writeopen){
    if(killed(pr)){
      release(&pi->lock);
      return -1;
    }
    sleep(&pi->nread, &pi->lock);
  }
  int was_full = (pi->nwrite == pi->nread + PIPE_TOTAL);  // writer sleeping?

  while(i < n){
    if(pi->nread == pi->nwrite)
      break;
    uint avail     = pi->nwrite - pi->nread;
    uint tail_slot = pi->nread % PIPE_TOTAL;
    uint till_wrap = PIPE_TOTAL - tail_slot;
    int to_copy = (int)umin((uint)(n - i), umin(avail, till_wrap));
    if(copyout(pr->pagetable, addr + i, &pi->data[tail_slot], to_copy) == -1){
      if(i == 0) i = -1;
      break;
    }
    pi->nread += to_copy;
    i         += to_copy;
  }
  if(was_full && i > 0)
    wakeup(&pi->nwrite);
  release(&pi->lock);
  return i;
}
#endif
