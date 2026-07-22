// RealWorld corpus (Track 4), multi-TU program `logger`: the implementation
// TU. A file-static counter and a printf-based logger called across the TU
// boundary by log_main.c.
#include <stdio.h>

static int count = 0;

void log_msg(const char *m) {
  count++;
  printf("[%d] %s\n", count, m);
}
