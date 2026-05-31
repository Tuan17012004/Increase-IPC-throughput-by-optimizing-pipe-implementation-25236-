// IPC throughput benchmark using xv6 pipe implementation.
//
// Parent forks a child, then transfers `total_bytes` of data over a pipe
// in chunks of `chunk_bytes`. Elapsed time is measured with uptime() ticks
// (xv6 default is 10 ticks/second), and IPC throughput is reported in B/s
// and KB/s.
//
// Usage:
//   pipetest                              # default 8MB, 256B chunks
//   pipetest <total_bytes>                # custom size, default chunk
//   pipetest <total_bytes> <chunk_bytes>  # custom size and chunk

#include "kernel/types.h"
#include "user/user.h"

#define DEFAULT_TOTAL_BYTES (8 * 1024 * 1024)
#define DEFAULT_CHUNK_BYTES 256
#define TICKS_PER_SEC       10

static int
imin(int a, int b)
{
  return a < b ? a : b;
}

static void
run_child_reader(int rfd, int wfd, char *buf, int chunk_bytes, int total_bytes)
{
  int got = 0;
  int n;

  close(wfd);
  while(got < total_bytes){
    n = read(rfd, buf, imin(chunk_bytes, total_bytes - got));
    if(n <= 0)
      break;
    got += n;
  }
  close(rfd);
  exit(0);
}

static int
run_parent_writer(int rfd, int wfd, char *buf, int chunk_bytes, int total_bytes)
{
  int sent = 0;
  int start_ticks, end_ticks, elapsed;

  close(rfd);

  start_ticks = uptime();
  while(sent < total_bytes){
    int n = write(wfd, buf, imin(chunk_bytes, total_bytes - sent));
    if(n <= 0){
      fprintf(2, "pipetest: write failed\n");
      close(wfd);
      wait(0);
      exit(1);
    }
    sent += n;
  }
  close(wfd);
  wait(0);
  end_ticks = uptime();

  elapsed = end_ticks - start_ticks;
  if(elapsed <= 0)
    elapsed = 1;
  return elapsed;
}

int
main(int argc, char *argv[])
{
  int total_bytes = DEFAULT_TOTAL_BYTES;
  int chunk_bytes = DEFAULT_CHUNK_BYTES;
  char *buf;
  int p[2];
  int pid;
  int elapsed_ticks;
  int throughput_bps;

  if(argc >= 2)
    total_bytes = atoi(argv[1]);
  if(argc >= 3)
    chunk_bytes = atoi(argv[2]);

  if(total_bytes <= 0 || chunk_bytes <= 0){
    fprintf(2, "usage: pipetest [total_bytes] [chunk_bytes]\n");
    exit(1);
  }

  buf = malloc(chunk_bytes);
  if(buf == 0){
    fprintf(2, "pipetest: malloc(%d) failed\n", chunk_bytes);
    exit(1);
  }
  memset(buf, 'a', chunk_bytes);

  if(pipe(p) < 0){
    fprintf(2, "pipetest: pipe failed\n");
    exit(1);
  }

  pid = fork();
  if(pid < 0){
    fprintf(2, "pipetest: fork failed\n");
    close(p[0]);
    close(p[1]);
    exit(1);
  }

  if(pid == 0){
    run_child_reader(p[0], p[1], buf, chunk_bytes, total_bytes);
    // not reached
  }

  elapsed_ticks = run_parent_writer(p[0], p[1], buf, chunk_bytes, total_bytes);
  throughput_bps = (total_bytes * TICKS_PER_SEC) / elapsed_ticks;

  printf("==== IPC throughput (xv6 pipe) ====\n");
  printf("  total bytes    : %d\n", total_bytes);
  printf("  chunk bytes    : %d\n", chunk_bytes);
  printf("  elapsed        : %d ticks (~%d ms)\n",
         elapsed_ticks, elapsed_ticks * (1000 / TICKS_PER_SEC));
  printf("  IPC throughput : %d B/s (%d KB/s)\n",
         throughput_bps, throughput_bps / 1024);
  printf("===================================\n");

  exit(0);
}
