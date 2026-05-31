// pipe_trace_demo.c — chứng minh đường đi write/read qua pipe
// Biên dịch với: make PIPE_VERSION=1 PIPE_TRACE=1
// Chạy trong QEMU: pipe_trace_demo
#include "kernel/types.h"
#include "user/user.h"

int
main(void)
{
  int fds[2];
  char buf[8];
  char msg[] = "HELLO";
  int n = 5;

  printf("\n");
  printf("============================================================\n");
  printf("  pipe_trace_demo  --  PIPE_VERSION=1, PIPE_TRACE=1\n");
  printf("  Muc tieu: chung minh duong di write/read qua pipe\n");
  printf("============================================================\n");

  pipe(fds);  // fds[0]=read-end, fds[1]=write-end

  // ---- WRITE PATH ----
  printf("\n>>> BUOC 1: write(fd[1], \"HELLO\", 5)\n");
  printf("    [user] ecall -> kernel...\n");
  write(fds[1], msg, n);
  printf("    [user] write() tra ve %d\n", n);

  // ---- READ PATH ----
  printf("\n>>> BUOC 2: read(fd[0], buf, 5)\n");
  printf("    [user] ecall -> kernel...\n");
  int r = read(fds[0], buf, n);
  buf[r] = '\0';
  printf("    [user] read() tra ve %d, nhan duoc: \"%s\"\n", r, buf);

  printf("\n============================================================\n");
  printf("  Ket qua: truyen \"%s\" thanh cong qua pipe\n", buf);
  printf("============================================================\n\n");

  close(fds[0]);
  close(fds[1]);
  exit(0);
}
