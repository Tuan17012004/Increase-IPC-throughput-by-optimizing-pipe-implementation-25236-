// pipe_wc.c — Word-count pipeline
//
// Simulates: generate_text | wc
//
// Parent generates total_bytes of synthetic text in chunk-size writes.
// Child reads and counts bytes / words / lines.
//
// Use case: demonstrates pipe IPC for streaming text processing.
// With v8 (8 KB buffer, bulk copy), the parent spends far less time
// sleeping on a full pipe, so text flows faster to the counting child.
//
// Usage:
//   pipe_wc                  # default 4 MB
//   pipe_wc <total_bytes>    # custom size
//   pipe_wc <total_bytes> <chunk_bytes>

#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define TICKS_PER_SEC  10
#define DEFAULT_TOTAL  (4 * 1024 * 1024)
#define DEFAULT_CHUNK  4096

// Text pattern: "aaaaaaaa bbbbbbbb cccccccc\n" (27 bytes, 3 words, 1 line)
#define PATTERN      "aaaaaaaa bbbbbbbb cccccccc\n"
#define PATTERN_LEN  27

static int imin(int a, int b) { return a < b ? a : b; }

int
main(int argc, char *argv[])
{
  int total = DEFAULT_TOTAL;
  int chunk = DEFAULT_CHUNK;
  if(argc >= 2) total = atoi(argv[1]);
  if(argc >= 3) chunk = atoi(argv[2]);
  if(total <= 0 || chunk <= 0){
    fprintf(2, "usage: pipe_wc [total_bytes] [chunk_bytes]\n");
    exit(1);
  }

  char *wbuf = malloc(chunk);
  char *rbuf = malloc(chunk);
  if(!wbuf || !rbuf){ fprintf(2, "pipe_wc: malloc failed\n"); exit(1); }

  // Fill write buffer with repeating text pattern
  int i;
  for(i = 0; i < chunk; ){
    int n = imin(PATTERN_LEN, chunk - i);
    memmove(wbuf + i, PATTERN, n);
    i += n;
  }

  int p[2];
  if(pipe(p) < 0){ fprintf(2, "pipe_wc: pipe failed\n"); exit(1); }

  int pid = fork();
  if(pid < 0){ fprintf(2, "pipe_wc: fork failed\n"); exit(1); }

  // ---- CHILD: count bytes, words, lines ----
  if(pid == 0){
    close(p[1]);
    int nbytes = 0, nwords = 0, nlines = 0, in_word = 0, n;
    while((n = read(p[0], rbuf, chunk)) > 0){
      int j;
      nbytes += n;
      for(j = 0; j < n; j++){
        char c = rbuf[j];
        if(c == '\n') nlines++;
        if(c == ' ' || c == '\n' || c == '\t'){
          in_word = 0;
        } else {
          if(!in_word){ nwords++; in_word = 1; }
        }
      }
    }
    close(p[0]);
    printf("  wc: %d lines  %d words  %d bytes\n", nlines, nwords, nbytes);
    free(rbuf);
    exit(0);
  }

  // ---- PARENT: generate and write text ----
  close(p[0]);
  int start = uptime();
  int sent = 0;
  while(sent < total){
    int n = write(p[1], wbuf, imin(chunk, total - sent));
    if(n <= 0){ fprintf(2, "pipe_wc: write error\n"); break; }
    sent += n;
  }
  close(p[1]);
  wait(0);
  int elapsed = uptime() - start;
  if(elapsed <= 0) elapsed = 1;

  int bps = (total * TICKS_PER_SEC) / elapsed;
  printf("==== pipe_wc: text processing pipeline ====\n");
  printf("  total bytes   : %d\n", total);
  printf("  chunk bytes   : %d\n", chunk);
  printf("  elapsed       : %d ticks (%d ms)\n", elapsed, elapsed * 100);
  printf("  throughput    : %d B/s (%d KB/s)\n", bps, bps / 1024);
  printf("===========================================\n");

  free(wbuf);
  exit(0);
}
