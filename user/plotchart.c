// Read IPC throughput results saved by pipebench and draw an
// ASCII horizontal bar chart on the console.
//
// Input file format (default "ipctp.txt"):
//   # comment line (optional, can repeat)
//   <label> <value>
//   <label> <value>
//   ...
//
// Usage:
//   plotchart [filename]

#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define DEFAULT_INPUT "ipctp.txt"
#define MAX_POINTS    32
#define BAR_WIDTH     50
#define HEADER_LINES  4
#define HEADER_LEN    128

struct point {
  int label;
  int value;
};

static int
read_all(int fd, char *buf, int max)
{
  int total = 0;
  int n;
  while(total < max - 1){
    n = read(fd, buf + total, max - 1 - total);
    if(n <= 0)
      break;
    total += n;
  }
  buf[total] = 0;
  return total;
}

static void
skip_ws(char **p)
{
  while(**p == ' ' || **p == '\t')
    (*p)++;
}

static void
skip_to_next_line(char **p)
{
  while(**p && **p != '\n')
    (*p)++;
  if(**p == '\n')
    (*p)++;
}

static int
parse_int(char **p)
{
  int n = 0, neg = 0;
  if(**p == '-'){
    neg = 1;
    (*p)++;
  }
  while(**p >= '0' && **p <= '9'){
    n = n * 10 + (**p - '0');
    (*p)++;
  }
  return neg ? -n : n;
}

int
main(int argc, char *argv[])
{
  char *fname = DEFAULT_INPUT;
  static char buf[8192];
  char header[HEADER_LEN];
  int fd, n;
  struct point pts[MAX_POINTS];
  int npts = 0;
  int max_val = 0;
  char *p;
  int i, j;

  if(argc >= 2)
    fname = argv[1];

  fd = open(fname, O_RDONLY);
  if(fd < 0){
    fprintf(2, "plotchart: cannot open %s\n", fname);
    exit(1);
  }

  n = read_all(fd, buf, sizeof(buf));
  close(fd);
  if(n <= 0){
    fprintf(2, "plotchart: %s is empty\n", fname);
    exit(1);
  }

  header[0] = 0;
  p = buf;
  while(*p && npts < MAX_POINTS){
    skip_ws(&p);
    if(!*p)
      break;
    if(*p == '\n'){
      p++;
      continue;
    }
    if(*p == '#'){
      // capture first comment line as header
      if(header[0] == 0){
        int hlen = 0;
        char *q = p + 1;
        if(*q == ' ')
          q++;
        while(*q && *q != '\n' && hlen < HEADER_LEN - 1)
          header[hlen++] = *q++;
        header[hlen] = 0;
      }
      skip_to_next_line(&p);
      continue;
    }
    pts[npts].label = parse_int(&p);
    skip_ws(&p);
    pts[npts].value = parse_int(&p);
    skip_to_next_line(&p);
    if(pts[npts].value > max_val)
      max_val = pts[npts].value;
    npts++;
  }

  if(npts == 0){
    fprintf(2, "plotchart: no data points in %s\n", fname);
    exit(1);
  }
  if(max_val <= 0)
    max_val = 1;

  printf("==== IPC throughput chart ====\n");
  printf("  source : %s\n", fname);
  if(header[0])
    printf("  meta   : %s\n", header);
  printf("  points : %d, max value : %d B/s\n", npts, max_val);
  printf("\n");
  printf("  %-12s %-14s  bar (1 # = %d B/s)\n",
         "label", "B/s", max_val / BAR_WIDTH > 0 ? max_val / BAR_WIDTH : 1);

  for(i = 0; i < npts; i++){
    int bar = (pts[i].value * BAR_WIDTH) / max_val;
    if(bar < 0)
      bar = 0;
    if(bar > BAR_WIDTH)
      bar = BAR_WIDTH;
    printf("  %-12d %-14d  ", pts[i].label, pts[i].value);
    for(j = 0; j < bar; j++)
      printf("#");
    printf("\n");
  }
  exit(0);
}
