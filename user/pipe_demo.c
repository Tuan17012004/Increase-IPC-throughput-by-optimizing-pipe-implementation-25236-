// pipe_demo.c — Bidirectional IPC: nội dung + tốc độ
//
// VẤN ĐỀ khi in từng vòng:
//   xv6 console ghi từng byte qua kernel spinlock → ~100 µs/ký tự.
//   In 200 ký tự/vòng ≈ 20 ms overhead, che khuất IPC hoàn toàn.
//   → v1 và v7 đều bị bottleneck bởi console → chỉ thấy ~2x.
//
// GIẢI PHÁP — 2 phase riêng biệt:
//
//   PHASE 1 (P1_ROUNDS = 10 vòng, chunk = P1_CHUNK = 64 B):
//     In đầy đủ nội dung từng vòng để chứng minh dữ liệu đúng.
//     64 B đủ nhỏ để in hết, số vòng ít → console overhead chấp nhận được.
//
//   PHASE 2 (P2_ROUNDS = 800 vòng, chunk = P2_CHUNK = 4096 B):
//     KHÔNG in từng vòng → IPC là bottleneck thật sự.
//     In 1 dòng progress mỗi 50 vòng (3 ký tự "#\n" ≈ 0.3 ms — không đáng kể).
//     v7: 4096 B vừa đúng buffer → 1 write, không block → ~0.6 ms/vòng.
//     v1: buffer 512 B → 8 lần fill+sleep/wakeup → ~15 ms/vòng.
//     → 800 vòng: v7 ≈ 0.5 s, v1 ≈ 12 s → thấy ~20x trực tiếp trên terminal.
//
// Chạy:  pipe_demo
//        pipe_demo <p2_rounds>    (tăng số vòng phase 2, mặc định 800)

#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define TICKS_PER_SEC   10

// Phase 1: chứng minh nội dung
#define P1_ROUNDS   10
#define P1_CHUNK    64        // đủ nhỏ để in hết mỗi byte

// Phase 2: đua tốc độ — IPC phải là bottleneck
#define P2_CHUNK    4096      // = buffer v7; v1 cần 8 lần fill+sleep
#define P2_DEFAULT  800       // số vòng phase 2
#define P2_PROGRESS 50        // in 1 dấu "#" mỗi N vòng (overhead nhỏ)

// ─── helpers ────────────────────────────────────────────────────────────────

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

// In 1 dòng bằng 1 write() syscall — không dùng printf() gọi write() từng byte
static void
emit(const char *buf, int len){ write(1, buf, len); }

// Ghi số nguyên vào buffer, trả về pointer sau
static char *
puti(char *p, int d, int w)
{
  char t[12]; int i = 0;
  if(d == 0) t[i++] = '0';
  while(d > 0){ t[i++] = '0' + d%10; d /= 10; }
  while(i < w) t[i++] = '0';
  while(i-- > 0) *p++ = t[i];
  return p;
}

static char *
puts2(char *p, const char *s){ while(*s) *p++ = *s++; return p; }

// ─── nội dung tin nhắn ──────────────────────────────────────────────────────

// Điền msg_len byte:  "P>C rNNNNNN tTTTT " (18 B) + fill_ch × (msg_len-18)
// Không có null, không có newline — buffer thô để truyền qua pipe
static void
make_msg(char *buf, int msg_len, char dir0, char dir1, int round, int tick, char fill)
{
  char *p = buf;
  *p++ = dir0; *p++ = '>'; *p++ = dir1; *p++ = ' '; *p++ = 'r';
  p = puti(p, round, 6);
  *p++ = ' '; *p++ = 't';
  p = puti(p, tick, 4);
  *p++ = ' ';
  // header dài 5+6+2+4+1 = 18 byte
  while(p < buf + msg_len) *p++ = fill;
}

// ─── Phase 1: in nội dung đầy đủ ───────────────────────────────────────────
//
// Mỗi vòng in 3 dòng:
//   SEND #01 [64B]: "P>C r000001 t0003 AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
//   RECV #01 [64B]: "C>P r000001 t0003 ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ"
//   >> OK  cumul=1280 B

static void
print_p1_round(int round, char *sm, char *rm, int n_sm, int n_rm, int ok, int cumul_b)
{
  char line[160]; char *p;

  p = line;
  p = puts2(p, "SEND #"); p = puti(p, round, 2);
  p = puts2(p, " ["); p = puti(p, n_sm, 1); p = puts2(p, "B]: \"");
  for(int i = 0; i < n_sm; i++) *p++ = sm[i];
  *p++ = '"'; *p++ = '\n';
  emit(line, p - line);

  p = line;
  p = puts2(p, "RECV #"); p = puti(p, round, 2);
  p = puts2(p, " ["); p = puti(p, n_rm, 1); p = puts2(p, "B]: \"");
  for(int i = 0; i < (n_rm < P1_CHUNK ? n_rm : P1_CHUNK); i++) *p++ = rm[i];
  *p++ = '"'; *p++ = '\n';
  emit(line, p - line);

  p = line;
  p = puts2(p, ok ? ">> OK  cumul=" : ">> ERR cumul=");
  p = puti(p, cumul_b, 1); p = puts2(p, " B\n\n");
  emit(line, p - line);
}

// ─── main ────────────────────────────────────────────────────────────────────

int
main(int argc, char *argv[])
{
  int p2_rounds = P2_DEFAULT;
  if(argc >= 2) p2_rounds = atoi(argv[1]);
  if(p2_rounds <= 0){
    write(2, "usage: pipe_demo [p2_rounds]\n", 28);
    exit(1);
  }

  // p2c: parent→child, c2p: child→parent
  int p2c[2], c2p[2];
  if(pipe(p2c) < 0 || pipe(c2p) < 0){
    write(2, "pipe_demo: pipe() failed\n", 24); exit(1);
  }

  int pid = fork();
  if(pid < 0){ write(2, "pipe_demo: fork() failed\n", 24); exit(1); }

  // ── CHILD: echo server ──────────────────────────────────────────────────────
  if(pid == 0){
    close(p2c[1]); close(c2p[0]);

    char *p1buf = malloc(P1_CHUNK);
    char *p2buf = malloc(P2_CHUNK);
    if(!p1buf || !p2buf){ write(2, "child: malloc\n", 14); exit(1); }

    // Phase 1: nhận P1_CHUNK, tạo reply C>P độc lập
    for(int i = 0; i < P1_ROUNDS; i++){
      if(readall(p2c[0], p1buf, P1_CHUNK) < P1_CHUNK) goto done;
      char tmp[P1_CHUNK];
      make_msg(tmp, P1_CHUNK, 'C', 'P', i+1, uptime(), 'Z');
      int w = 0;
      while(w < P1_CHUNK){
        int r = write(c2p[1], tmp+w, P1_CHUNK-w);
        if(r <= 0) goto done;
        w += r;
      }
    }

    // Phase 2: echo P2_CHUNK không print
    for(int i = 0; i < p2_rounds; i++){
      if(readall(p2c[0], p2buf, P2_CHUNK) < P2_CHUNK) break;
      make_msg(p2buf, P2_CHUNK, 'C', 'P', i+1, uptime(), 'Z');
      int w = 0;
      while(w < P2_CHUNK){
        int r = write(c2p[1], p2buf+w, P2_CHUNK-w);
        if(r <= 0) goto done;
        w += r;
      }
    }
  done:
    free(p1buf); free(p2buf);
    close(p2c[0]); close(c2p[1]);
    exit(0);
  }

  // ── PARENT ──────────────────────────────────────────────────────────────────
  close(p2c[0]); close(c2p[1]);

  char *p1s = malloc(P1_CHUNK);
  char *p1r = malloc(P1_CHUNK);
  char *p2s = malloc(P2_CHUNK);
  char *p2r = malloc(P2_CHUNK);
  if(!p1s || !p1r || !p2s || !p2r){
    write(2, "parent: malloc\n", 15);
    close(p2c[1]); close(c2p[0]); wait(0); exit(1);
  }

  // ─── header ───
  {
    char h[512]; char *p = h;
    p = puts2(p, "\n");
    p = puts2(p, "===========================================================\n");
    p = puts2(p, " pipe_demo  |  Bidirectional IPC  |  Content + Speed\n");
    p = puts2(p, "===========================================================\n");
    p = puts2(p, " PHASE 1: in noi dung day du  (");
    p = puti(p, P1_ROUNDS, 1); p = puts2(p, " vong x ");
    p = puti(p, P1_CHUNK, 1); p = puts2(p, " B)\n");
    p = puts2(p, " PHASE 2: dua toc do, KHONG in tung vong  (");
    p = puti(p, p2_rounds, 1); p = puts2(p, " vong x ");
    p = puti(p, P2_CHUNK, 1); p = puts2(p, " B)\n");
    p = puts2(p, " v1: buffer 512B → ");
    p = puti(p, P2_CHUNK/512, 1);
    p = puts2(p, " lan fill+sleep/wakeup/vong\n");
    p = puts2(p, " v7: buffer 4096B → 1 lan write, khong block/vong\n");
    p = puts2(p, "===========================================================\n\n");
    emit(h, p - h);
  }

  // ─── PHASE 1: in nội dung ───
  {
    char h[80]; char *p = h;
    p = puts2(p, "--- PHASE 1: Content Verification ---\n\n");
    emit(h, p - h);
  }

  int p1_ok = 0;
  for(int i = 0; i < P1_ROUNDS; i++){
    make_msg(p1s, P1_CHUNK, 'P', 'C', i+1, uptime(), 'A');

    int w = 0;
    while(w < P1_CHUNK){
      int r = write(p2c[1], p1s+w, P1_CHUNK-w);
      if(r <= 0) goto p2start;
      w += r;
    }
    int n = readall(c2p[0], p1r, P1_CHUNK);
    int ok = (n == P1_CHUNK && p1r[0]=='C' && p1r[2]=='P');
    if(ok) p1_ok++;
    print_p1_round(i+1, p1s, p1r, P1_CHUNK, n, ok, (i+1)*P1_CHUNK*2);
  }
  {
    char s[80]; char *p = s;
    p = puts2(p, "Phase 1 done: ");
    p = puti(p, p1_ok, 1); p = puts2(p, "/");
    p = puti(p, P1_ROUNDS, 1); p = puts2(p, " vong dung\n\n");
    emit(s, p - s);
  }

p2start:
  // ─── PHASE 2: đua tốc độ ───
  {
    char h[160]; char *p = h;
    p = puts2(p, "--- PHASE 2: Speed Race (");
    p = puti(p, p2_rounds, 1); p = puts2(p, " vong x ");
    p = puti(p, P2_CHUNK, 1); p = puts2(p, " B, '#' moi ");
    p = puti(p, P2_PROGRESS, 1); p = puts2(p, " vong) ---\n");
    emit(h, p - h);
  }

  memset(p2s, 'X', P2_CHUNK);   // nội dung fill 'X'
  int p2_ok = 0;
  int t0 = uptime();

  for(int i = 0; i < p2_rounds; i++){
    make_msg(p2s, P2_CHUNK, 'P', 'C', i+1, uptime(), 'A');

    int w = 0;
    while(w < P2_CHUNK){
      int r = write(p2c[1], p2s+w, P2_CHUNK-w);
      if(r <= 0) goto p2done;
      w += r;
    }
    int n = readall(c2p[0], p2r, P2_CHUNK);
    if(n < P2_CHUNK) break;
    if(p2r[0]=='C' && p2r[2]=='P') p2_ok++;

    // Progress: in "#\n" mỗi P2_PROGRESS vòng — overhead ~0.3 ms (không đáng kể)
    if((i+1) % P2_PROGRESS == 0){
      char mark[64]; char *p = mark;
      p = puts2(p, "  #"); p = puti(p, i+1, 4);
      p = puts2(p, " vong | ");
      p = puti(p, (i+1) * P2_CHUNK * 2 / 1024, 6);
      p = puts2(p, " KB | t=");
      p = puti(p, (uptime()-t0)*100, 1);
      p = puts2(p, " ms\n");
      emit(mark, p - mark);
    }
  }

p2done:
  close(p2c[1]); close(c2p[0]);
  wait(0);

  int elapsed = uptime() - t0;
  if(elapsed <= 0) elapsed = 1;

  int p2_bytes = p2_ok * P2_CHUNK * 2;
  int kbps = (p2_bytes / elapsed) * TICKS_PER_SEC / 1024;
  int mbps_int  = kbps / 1024;
  int mbps_frac = (kbps % 1024) * 10 / 1024;

  free(p1s); free(p1r); free(p2s); free(p2r);

  {
    char f[512]; char *p = f;
    p = puts2(p, "\n===========================================================\n");
    p = puts2(p, "  KET QUA TONG HOP\n");
    p = puts2(p, "===========================================================\n");
    p = puts2(p, "  Phase 1 content : ");
    p = puti(p, p1_ok, 1); p = puts2(p, "/");
    p = puti(p, P1_ROUNDS, 1); p = puts2(p, " vong dung\n");
    p = puts2(p, "  Phase 2 rounds  : ");
    p = puti(p, p2_ok, 1); p = puts2(p, "/");
    p = puti(p, p2_rounds, 1); p = puts2(p, "\n");
    p = puts2(p, "  Tong du lieu    : ");
    p = puti(p, p2_bytes/1024, 1); p = puts2(p, " KB  (");
    p = puti(p, p2_bytes/1024/1024, 1); p = puts2(p, " MB)\n");
    p = puts2(p, "  Thoi gian P2    : ");
    p = puti(p, elapsed, 1); p = puts2(p, " ticks  (");
    p = puti(p, elapsed * 100, 1); p = puts2(p, " ms)\n");
    p = puts2(p, "  Throughput      : ");
    p = puti(p, kbps, 1); p = puts2(p, " KB/s  (");
    p = puti(p, mbps_int, 1); p = puts2(p, ".");
    p = puti(p, mbps_frac, 1); p = puts2(p, " MB/s)\n");
    p = puts2(p, "===========================================================\n");
    emit(f, p - f);
  }

  exit(0);
}
