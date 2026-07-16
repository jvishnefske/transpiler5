// Companion translation unit for system-headers-multi-tu.c. Includes the
// same system header (skipped per-TU) and the same project header (imported;
// the struct shape dedups with the main TU's import).
#include <stdio.h>
#include "helper.h"

int point_sum(struct Point p) {
  printf("summing\n");
  return p.x + p.y;
}
