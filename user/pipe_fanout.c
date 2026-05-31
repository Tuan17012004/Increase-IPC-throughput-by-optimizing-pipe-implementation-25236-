// pipe_fanout.c — Fan-out: 1 writer, N concurrent readers
//
// Simulates: source | tee reader0 reader1 reader2
//
// One parent process broadcasts the same data to N children, each reading
// from its own dedicated pipe. The parent writes to pipe[0], pipe[1], ...
// pipe[N-1] in round-robin order.
//
//   Parent ─── pipe[0] ──► Child 0  (reads total_bytes/N)
//          ─── pipe[1] ──► Child 1  (reads total_bytes/N)
//              ...
//          ─── pipe[N-1] ► Child N-1
//
// Use case: parallel distribution — e.g. log fan-out, data sharding.
// With v8's 8 KB buffer, the parent can stay further ahead of each reader,
// filling 2× as much data before blocking on any single pipe.
//
// Usage:
//   pipe_fanout                            # 3 readers, 2 MB each
//   pipe_fanout <n_readers>                # up to 4 readers
//   pipe_fanout <n_readers> <total_bytes>  # total_bytes per reader

#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define TICKS_PER_SEC   10
#define DEFAULT_READERS 3
#define DEFAULT_TOTAL   (2 * 1024 * 1024)
#define MAX_READERS     4
#define CHUNK           4096

static int imin(int a, int b) { return a < b ? a : b; }

int
main(int argc, char *argv[])
{
  int nreaders = DEFAULT_READERS;
  int per_reader = DEFAULT_TOTAL;
  if(argc >= 2) nreaders = atoi(argv[1]);
  if(argc >= 3) per_reader = atoi(argv[2]);
  if(nreaders <= 0 || nreaders > MAX_READERS || per_reader <= 0){
    fprintf(2, "usage: pipe_fanout [n_readers 1-%d] [bytes_per_reader]\n", MAX_READERS);
    exit(1);
  }

  // Create N pipes upfront
  int p[MAX_READERS][2];
  int i;
  for(i = 0; i < nreaders; i++){
    if(pipe(p[i]) < 0){
      fprintf(2, "pipe_fanout: pipe failed\n"); exit(1);
    }
  }

  char *buf = malloc(CHUNK);
  if(!buf){ fprintf(2, "pipe_fanout: malloc failed\n"); exit(1); }
  memset(buf, 'x', CHUNK);

  // Fork N reader children
  for(i = 0; i < nreaders; i++){
    int pid = fork();
    if(pid < 0){ fprintf(2, "pipe_fanout: fork failed\n"); exit(1); }
    if(pid == 0){
      // Child i: close all write ends and other pipes' read ends
      int j;
      for(j = 0; j < nreaders; j++){
        close(p[j][1]);          // all write ends
        if(j != i) close(p[j][0]);  // other read ends
      }
      // Read exactly per_reader bytes from p[i][0]
      int got = 0, n;
      while(got < per_reader){
        n = read(p[i][0], buf, imin(CHUNK, per_reader - got));
        if(n <= 0) break;
        got += n;
      }
      close(p[i][0]);
      free(buf);
      exit(0);
    }
  }

  // Parent: close all read ends (only writes)
  for(i = 0; i < nreaders; i++)
    close(p[i][0]);

  // Write per_reader bytes to each pipe in round-robin
  int start = uptime();
  int sent[MAX_READERS];
  for(i = 0; i < nreaders; i++) sent[i] = 0;

  int total = nreaders * per_reader;
  int done = 0;
  while(done < total){
    for(i = 0; i < nreaders; i++){
      if(sent[i] >= per_reader) continue;
      int to_write = imin(CHUNK, per_reader - sent[i]);
      int n = write(p[i][1], buf, to_write);
      if(n <= 0){ fprintf(2, "pipe_fanout: write to pipe%d failed\n", i); break; }
      sent[i] += n;
      done += n;
    }
  }
  // Close all write ends to signal EOF
  for(i = 0; i < nreaders; i++)
    close(p[i][1]);

  // Wait for all children
  for(i = 0; i < nreaders; i++)
    wait(0);

  int elapsed = uptime() - start;
  if(elapsed <= 0) elapsed = 1;

  int bps = (total * TICKS_PER_SEC) / elapsed;
  printf("==== pipe_fanout: 1 writer -> %d readers ====\n", nreaders);
  printf("  readers       : %d\n", nreaders);
  printf("  bytes/reader  : %d\n", per_reader);
  printf("  total bytes   : %d\n", total);
  printf("  elapsed       : %d ticks (%d ms)\n", elapsed, elapsed * 100);
  printf("  throughput    : %d B/s (%d KB/s)\n", bps, bps / 1024);
  printf("  per-pipe bps  : %d B/s\n", bps / nreaders);
  printf("=============================================\n");

  free(buf);
  exit(0);
}
