#include "kernel/types.h"
#include "user/user.h"

int
main(void)
{
  int p[2];
  int pid;
  char buf[64];
  int n;
  char *m1 = "message 1 from parent\n";
  char *m2 = "message 2 from parent\n";
  char *m3 = "message 3 from parent\n";

  printf("[user] step 1: call pipe(p)\n");

  if(pipe(p) < 0){
    fprintf(2, "pipeexample: pipe failed\n");
    exit(1);
  }
  printf("[user] pipe() success: p[0]=%d (read end), p[1]=%d (write end)\n", p[0], p[1]);

  printf("[user] step 2: fork()\n");
  pid = fork();
  if(pid < 0){
    fprintf(2, "pipeexample: fork failed\n");
    close(p[0]);
    close(p[1]);
    exit(1);
  }

  if(pid == 0){
    printf("[child] start: close write end p[1]=%d\n", p[1]);
    close(p[1]);
    while((n = read(p[0], buf, sizeof(buf) - 1)) > 0){
      buf[n] = 0;
      printf("[child] read %d bytes from p[0]=%d: %s", n, p[0], buf);
    }
    printf("[child] read returned %d -> EOF (writer closed)\n", n);
    printf("[child] close read end p[0]=%d\n", p[0]);
    close(p[0]);
    exit(0);
  }

  printf("[parent] start: close read end p[0]=%d\n", p[0]);
  close(p[0]);

  n = write(p[1], m1, strlen(m1));
  printf("[parent] write #1 -> %d bytes to p[1]=%d\n", n, p[1]);
  n = write(p[1], m2, strlen(m2));
  printf("[parent] write #2 -> %d bytes to p[1]=%d\n", n, p[1]);
  n = write(p[1], m3, strlen(m3));
  printf("[parent] write #3 -> %d bytes to p[1]=%d\n", n, p[1]);

  printf("[parent] close write end p[1]=%d\n", p[1]);
  close(p[1]);

  printf("[parent] wait child\n");
  wait(0);
  printf("[parent] done\n");
  exit(0);
}
