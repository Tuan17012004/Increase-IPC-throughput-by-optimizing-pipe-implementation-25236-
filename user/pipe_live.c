// pipe_live.c — Real-time pipe throughput display
//
// Hai tien trinh giao tiep qua pipe, hien thi toc do LIVE moi tick (100ms):
//
//   fork() tao ra 2 tien trinh:
//
//   Parent (WRITER) ──[pipe]──► Child (READER)
//        ghi lien tuc               doc + in toc do moi tick
//
// Usage:
//   pipe_live                    # 16 MB
//   pipe_live <total_MB>
//   pipe_live <total_MB> <chunk_bytes>

#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define TICKS_PER_SEC   10
#define DEFAULT_MB      16
#define DEFAULT_CHUNK   4096
#define BAR_WIDTH       28

static int imin(int a, int b) { return a < b ? a : b; }

static void
print_bar(int pct)
{
  int filled = (pct * BAR_WIDTH) / 100;
  int i;
  printf("[");
  for(i = 0; i < BAR_WIDTH; i++){
    if(i < filled)        printf("=");
    else if(i == filled)  printf(">");
    else                  printf(" ");
  }
  printf("]");
}

static void
print_kb(int bytes)
{
  int kb = bytes / 1024;
  if(kb >= 1024)
    printf("%d MB", kb / 1024);
  else
    printf("%d KB", kb);
}

int
main(int argc, char *argv[])
{
  int total_mb = DEFAULT_MB;
  int chunk    = DEFAULT_CHUNK;
  if(argc >= 2) total_mb = atoi(argv[1]);
  if(argc >= 3) chunk    = atoi(argv[2]);
  if(total_mb <= 0 || chunk <= 0){
    fprintf(2, "usage: pipe_live [total_MB] [chunk_bytes]\n");
    exit(1);
  }

  int total = total_mb * 1024 * 1024;

  char *buf = malloc(chunk);
  if(!buf){ fprintf(2, "malloc failed\n"); exit(1); }

  int p[2];
  if(pipe(p) < 0){ fprintf(2, "pipe failed\n"); exit(1); }

  int parent_pid = getpid();

  printf("\n");
  printf("  ===================================================\n");
  printf("     pipe_live: real-time IPC throughput monitor\n");
  printf("  ===================================================\n");
  printf("  total: %d MB  |  chunk: %d B\n", total_mb, chunk);
  printf("\n");
  printf("  Tao 2 tien trinh bang fork()...\n");
  printf("  WRITER [PID %d] --[pipe]--> READER [child]\n", parent_pid);
  printf("\n");
  printf("  tick | progress                           | pct | KB/s\n");
  printf("  -----|------------------------------------|----|------\n");

  int pid = fork();   // <<< TAO TIEN TRINH MOI
  if(pid < 0){ fprintf(2, "fork failed\n"); exit(1); }

  // ═══════════════════════════════════════════════════════════════
  // TIEN TRINH CON (READER) — doc pipe + in live stats
  // ═══════════════════════════════════════════════════════════════
  if(pid == 0){
    close(p[1]);

    int total_read = 0;
    int last_tick  = uptime();
    int tick_bytes = 0;
    int tick_num   = 0;

    while(total_read < total){
      int n = read(p[0], buf, imin(chunk, total - total_read));
      if(n <= 0) break;
      total_read += n;
      tick_bytes += n;

      int now = uptime();
      if(now != last_tick){
        tick_num++;
        int kbps = (tick_bytes * TICKS_PER_SEC) / 1024;
        int pct  = (total_read * 100) / total;

        printf("  %d | ", tick_num);
        print_bar(pct);
        printf(" | %d%% | %d KB/s | ", pct, kbps);
        print_kb(total_read);
        printf("\n");

        tick_bytes = 0;
        last_tick  = now;
      }
    }

    // Dong cuoi: du lieu con lai trong tick cuoi cung
    if(tick_bytes > 0){
      tick_num++;
      int kbps = (tick_bytes * TICKS_PER_SEC) / 1024;
      printf("  %d | ", tick_num);
      print_bar(100);
      printf(" | 100%% | %d KB/s | ", kbps);
      print_kb(total_read);
      printf("\n");
    }

    close(p[0]);
    free(buf);
    exit(0);
  }

  // ═══════════════════════════════════════════════════════════════
  // TIEN TRINH CHA (WRITER) — ghi pipe lien tuc
  // ═══════════════════════════════════════════════════════════════
  close(p[0]);
  memset(buf, 'W', chunk);

  int start = uptime();
  int sent  = 0;
  while(sent < total){
    int n = write(p[1], buf, imin(chunk, total - sent));
    if(n <= 0) break;
    sent += n;
  }
  close(p[1]);   // dong pipe -> con nhan EOF -> thoat
  wait(0);       // cho con in xong

  int elapsed = uptime() - start;
  if(elapsed <= 0) elapsed = 1;
  int avg_kbps = (total / 1024 * TICKS_PER_SEC) / elapsed;

  printf("  -----|------------------------------------|----|------\n");
  printf("\n");
  printf("  Ket qua:\n");
  printf("    tong du lieu : %d MB\n", total_mb);
  printf("    thoi gian    : %d ticks (%d ms)\n", elapsed, elapsed * 100);
  printf("    trung binh   : %d KB/s", avg_kbps);
  if(avg_kbps >= 1024)
    printf(" (%d MB/s)", avg_kbps / 1024);
  printf("\n");
  printf("\n");

  free(buf);
  exit(0);
}
