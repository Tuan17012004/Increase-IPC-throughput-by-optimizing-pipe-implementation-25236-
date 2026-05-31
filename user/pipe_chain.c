// pipe_chain.c — 3-stage transform pipeline
//
// Simulates: source | cipher | verify   (like: cmd1 | cmd2 | cmd3)
//
// Three processes connected by two pipes:
//
//   Stage 0 (generator)  ─── pipe0 ──► Stage 1 (XOR cipher) ─── pipe1 ──► Stage 2 (verifier)
//
//   Stage 0: writes bytes [0,1,2,...,255,0,1,...] cyclically to pipe0
//   Stage 1: reads from pipe0, XORs each byte with KEY (0x5A), writes to pipe1
//   Stage 2: reads from pipe1, verifies each byte == (expected ^ KEY), counts errors
//
// Use case: multi-hop data transformation — e.g. compress | encrypt | transmit.
// With v8, both pipes have 8 KB buffers, so each stage rarely stalls waiting
// for the next stage to drain — dramatically reducing inter-stage bubbles.
//
// Usage:
//   pipe_chain                           # 4 MB, 4096 B chunks
//   pipe_chain <total_bytes>
//   pipe_chain <total_bytes> <chunk>

#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define TICKS_PER_SEC  10
#define DEFAULT_TOTAL  (4 * 1024 * 1024)
#define DEFAULT_CHUNK  4096
#define KEY            0x5A

static int imin(int a, int b) { return a < b ? a : b; }

int
main(int argc, char *argv[])
{
  int total = DEFAULT_TOTAL;
  int chunk = DEFAULT_CHUNK;
  if(argc >= 2) total = atoi(argv[1]);
  if(argc >= 3) chunk = atoi(argv[2]);
  if(total <= 0 || chunk <= 0){
    fprintf(2, "usage: pipe_chain [total_bytes] [chunk_bytes]\n");
    exit(1);
  }

  // Create both pipes before forking so all children inherit them.
  int p0[2], p1[2];
  if(pipe(p0) < 0 || pipe(p1) < 0){
    fprintf(2, "pipe_chain: pipe failed\n"); exit(1);
  }

  // ---- Fork Stage 2 (verifier / sink) ----
  int pid2 = fork();
  if(pid2 < 0){ fprintf(2, "pipe_chain: fork2 failed\n"); exit(1); }
  if(pid2 == 0){
    close(p0[0]); close(p0[1]); close(p1[1]);   // only needs p1[0]
    char *buf = malloc(chunk);
    if(!buf){ fprintf(2, "stage2: malloc failed\n"); exit(1); }
    int got = 0, errors = 0, n;
    while((n = read(p1[0], buf, chunk)) > 0){
      int i;
      for(i = 0; i < n; i++){
        // expected: ((got+i) % 256) ^ KEY
        unsigned char expected = (unsigned char)(((got + i) % 256) ^ KEY);
        if((unsigned char)buf[i] != expected) errors++;
      }
      got += n;
    }
    close(p1[0]);
    if(errors == 0)
      printf("  stage2: OK  received %d bytes, 0 errors\n", got);
    else
      printf("  stage2: FAIL  received %d bytes, %d errors\n", got, errors);
    free(buf);
    exit(errors > 0 ? 1 : 0);
  }

  // ---- Fork Stage 1 (XOR cipher / transformer) ----
  int pid1 = fork();
  if(pid1 < 0){ fprintf(2, "pipe_chain: fork1 failed\n"); exit(1); }
  if(pid1 == 0){
    close(p0[1]); close(p1[0]);   // reads p0[0], writes p1[1]
    char *buf = malloc(chunk);
    if(!buf){ fprintf(2, "stage1: malloc failed\n"); exit(1); }
    int n;
    while((n = read(p0[0], buf, chunk)) > 0){
      int i;
      for(i = 0; i < n; i++)
        buf[i] ^= KEY;
      int written = 0;
      while(written < n){
        int w = write(p1[1], buf + written, n - written);
        if(w <= 0) break;
        written += w;
      }
    }
    close(p0[0]); close(p1[1]);
    free(buf);
    exit(0);
  }

  // ---- Stage 0: generator (main process) ----
  close(p0[0]); close(p1[0]); close(p1[1]);   // only needs p0[1]
  char *buf = malloc(chunk);
  if(!buf){ fprintf(2, "stage0: malloc failed\n"); exit(1); }

  int start = uptime();
  int sent = 0;
  while(sent < total){
    int to_send = imin(chunk, total - sent);
    int i;
    for(i = 0; i < to_send; i++)
      buf[i] = (char)((sent + i) % 256);
    int n = write(p0[1], buf, to_send);
    if(n <= 0){ fprintf(2, "stage0: write error\n"); break; }
    sent += n;
  }
  close(p0[1]);
  wait(0); wait(0);   // wait for both stage1 and stage2
  int elapsed = uptime() - start;
  if(elapsed <= 0) elapsed = 1;

  int bps = (total * TICKS_PER_SEC) / elapsed;
  printf("==== pipe_chain: 3-stage transform pipeline ====\n");
  printf("  stages        : gen -> XOR(0x%x) -> verify\n", KEY);
  printf("  total bytes   : %d\n", total);
  printf("  chunk bytes   : %d\n", chunk);
  printf("  elapsed       : %d ticks (%d ms)\n", elapsed, elapsed * 100);
  printf("  throughput    : %d B/s (%d KB/s)\n", bps, bps / 1024);
  printf("=================================================\n");

  free(buf);
  exit(0);
}
