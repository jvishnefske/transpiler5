// RealWorld corpus (Track 4), multi-TU program `calc`: the driver TU.
// Calls the primitives defined in calc_ops.c across the TU boundary.
#include <stdio.h>

int add(int, int);
int sub(int, int);
int mul(int, int);

int main(void) {
  int r = add(mul(3, 4), sub(10, 7));
  printf("%d\n", r);
  return 0;
}
