// pipe_pingpong.c — Bidirectional round-trip IPC
//
// Simulates: RPC / request-response (client-server over pipe)
//
// Two processes communicate over TWO pipes (full-duplex):
//
//   Parent ──► p2c[1] ──► p2c[0] ──► Child  (parent sends request)
//   Parent ◄── c2p[0] ◄── c2p[1] ◄── Child  (child echoes response)
//
// Parent sends a msg_size-byte request, child reads it and echoes it back.
// Repeat for n_rounds. Measures total time → derive round-trip throughput.
//
// Use case: RPC, event-driven IPC, shell pipelines where output feeds back.
// This stresses wakeup latency most directly: each round needs 2 wakeups
// (parent wakes child, child wakes parent). v6's priority boost ensures the
// woken process runs on the NEXT scheduler tick instead of waiting its turn.
//
// Usage:
//   pipe_pingpong                         # 10000 rounds, 512 B
//   pipe_pingpong <n_rounds>
//   pipe_pingpong <n_rounds> <msg_size>

#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define TICKS_PER_SEC    10
#define DEFAULT_ROUNDS   10000
#define DEFAULT_MSG      512

// Read exactly n bytes, retrying until done or error.
static int
readall(int fd, char *buf, int n)
{
  int got = 0;
  while(got < n){
    int r = read(fd, buf + got, n - got);
    if(r <= 0) return got;
    got += r;
  }
  return got;
}

int
main(int argc, char *argv[])
{
  int rounds = DEFAULT_ROUNDS;
  int msgsize = DEFAULT_MSG;
  if(argc >= 2) rounds = atoi(argv[1]);
  if(argc >= 3) msgsize = atoi(argv[2]);
  if(rounds <= 0 || msgsize <= 0){
    fprintf(2, "usage: pipe_pingpong [n_rounds] [msg_size_bytes]\n");
    exit(1);
  }

  // p2c: parent → child (request channel)
  // c2p: child → parent (response channel)
  int p2c[2], c2p[2];
  if(pipe(p2c) < 0 || pipe(c2p) < 0){
    fprintf(2, "pipe_pingpong: pipe failed\n"); exit(1);
  }

  char *buf = malloc(msgsize);
  if(!buf){ fprintf(2, "pipe_pingpong: malloc failed\n"); exit(1); }
  memset(buf, 'P', msgsize);

  int pid = fork();
  if(pid < 0){ fprintf(2, "pipe_pingpong: fork failed\n"); exit(1); }

  // ---- CHILD: echo server ----
  if(pid == 0){
    close(p2c[1]); close(c2p[0]);  // reads p2c[0], writes c2p[1]
    char *rbuf = malloc(msgsize);
    if(!rbuf){ fprintf(2, "child: malloc failed\n"); exit(1); }
    int i;
    for(i = 0; i < rounds; i++){
      int n = readall(p2c[0], rbuf, msgsize);
      if(n <= 0) break;
      // Echo back the same bytes
      int written = 0;
      while(written < n){
        int w = write(c2p[1], rbuf + written, n - written);
        if(w <= 0) goto done;
        written += w;
      }
    }
  done:
    close(p2c[0]); close(c2p[1]);
    free(rbuf);
    exit(0);
  }

  // ---- PARENT: request sender ----
  close(p2c[0]); close(c2p[1]);  // writes p2c[1], reads c2p[0]

  int start = uptime();
  int i;
  for(i = 0; i < rounds; i++){
    // Send request
    int written = 0;
    while(written < msgsize){
      int w = write(p2c[1], buf + written, msgsize - written);
      if(w <= 0){ fprintf(2, "pipe_pingpong: write failed at round %d\n", i); goto finish; }
      written += w;
    }
    // Wait for echo
    int n = readall(c2p[0], buf, msgsize);
    if(n < msgsize){ fprintf(2, "pipe_pingpong: short read at round %d\n", i); goto finish; }
  }
finish:
  close(p2c[1]); close(c2p[0]);
  wait(0);
  int elapsed = uptime() - start;
  if(elapsed <= 0) elapsed = 1;

  int total_bytes = rounds * msgsize * 2;   // each round: 1 send + 1 recv
  int bps = (total_bytes * TICKS_PER_SEC) / elapsed;
  // Latency per round: elapsed ticks * 100ms/tick / rounds
  int rtt_ms  = (elapsed * 100) / rounds;          // whole ms
  int rtt_frac = ((elapsed * 100) % rounds) * 100 / rounds;  // centiseconds

  printf("==== pipe_pingpong: round-trip IPC ====\n");
  printf("  rounds        : %d\n", rounds);
  printf("  msg size      : %d B\n", msgsize);
  printf("  total bytes   : %d\n", total_bytes);
  printf("  elapsed       : %d ticks (%d ms)\n", elapsed, elapsed * 100);
  printf("  throughput    : %d B/s (%d KB/s)\n", bps, bps / 1024);
  printf("  ops/sec       : %d\n", (rounds * TICKS_PER_SEC) / elapsed);
  printf("  ~latency/rtt  : %d.%d ms\n", rtt_ms, rtt_frac);
  printf("=======================================\n");

  free(buf);
  exit(0);
}
