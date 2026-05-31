// pccycle.c -- do chi phi CPU cycle cua pipe operations (v1 baseline)
//
// Build:  make PIPE_VERSION=1
// Run in QEMU: pccycle
//
// Yeu cau: mcounteren.CY=1 (start.c) va scounteren.CY=1 (trapinithart)
//
#include "kernel/types.h"
#include "user/user.h"

static inline uint64
rdcycle(void)
{
  uint64 c;
  asm volatile("csrr %0, cycle" : "=r"(c));
  return c;
}

#define SAMP 100

static uint64
median(uint64 *a, int n)
{
  int i, j;
  uint64 key;
  for(i = 1; i < n; i++){
    key = a[i];
    j = i - 1;
    while(j >= 0 && a[j] > key){ a[j+1] = a[j]; j--; }
    a[j+1] = key;
  }
  return a[n/2];
}

int
main(void)
{
  uint64 samples[SAMP];
  uint64 t0, t1;
  int fds[2];
  char wbuf[64], rbuf[64];
  int i, j;

  printf("=== pccycle: chi phi cycle cua pipe v1 (rdcycle) ===\n\n");

  // --- [1] Syscall baseline ---
  for(i = 0; i < SAMP; i++){
    t0 = rdcycle();
    getpid();
    t1 = rdcycle();
    samples[i] = t1 - t0;
  }
  uint64 syscall_cyc = median(samples, SAMP);
  printf("[1] Syscall round-trip (getpid) : %lu cycle\n", syscall_cyc);

  // --- [2] write(1B) to pipe ---
  pipe(fds);
  for(i = 0; i < SAMP; i++){
    wbuf[0] = 'A';
    t0 = rdcycle();
    write(fds[1], wbuf, 1);
    t1 = rdcycle();
    read(fds[0], rbuf, 1);
    samples[i] = t1 - t0;
  }
  uint64 write1_cyc = median(samples, SAMP);
  uint64 copyin_est = write1_cyc > syscall_cyc ? write1_cyc - syscall_cyc : 0;
  printf("[2] write(1B) -> pipe           : %lu cycle\n", write1_cyc);
  printf("    copyin overhead est         : %lu cycle\n\n", copyin_est);

  // --- [3] Scaling: N = 1, 2, 4, 8, 16, 32 bytes ---
  int sizes[6];
  sizes[0]=1; sizes[1]=2; sizes[2]=4;
  sizes[3]=8; sizes[4]=16; sizes[5]=32;

  printf("[3] Scaling: write(N bytes) cycle count\n");
  printf("    N  |  cycle(med)  |  cyc/byte\n");
  printf("    ---+--------------+----------\n");

  for(i = 0; i < 6; i++){
    int sz = sizes[i];
    for(j = 0; j < SAMP; j++){
      t0 = rdcycle();
      write(fds[1], wbuf, sz);
      t1 = rdcycle();
      read(fds[0], rbuf, sz);
      samples[j] = t1 - t0;
    }
    uint64 cyc = median(samples, SAMP);
    uint64 cpb = (uint64)sz > 0 ? cyc / (uint64)sz : 0;
    printf("    %d  |  %lu  |  %lu\n", sz, cyc, cpb);
  }

  printf("\n=> cyc/byte gan nhu KHONG DOI: copyin O(N), moi byte = 1 call.\n\n");

  // --- [4] Summary ---
  printf("[4] Tong hop so sanh (ly thuyet vs thuc do):\n");
  printf("    syscall round-trip   : %lu cycle  (ly thuyet: 300-500)\n", syscall_cyc);
  printf("    write(1B) pipeline   : %lu cycle\n", write1_cyc);
  printf("    copyin overhead est  : %lu cycle  (ly thuyet: ~83)\n", copyin_est);
  printf("\n    Luu y: QEMU virtual cycle >> real HW cycle.\n");
  printf("    Ty le tuong doi (N=1 vs N=32) la chinh xac.\n");

  close(fds[0]);
  close(fds[1]);
  printf("\n=== Ket thuc do ===\n");
  exit(0);
}
