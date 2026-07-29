// RealWorld C++ corpus (FR-46), project `shapes`: driver TU.
//
// Exercises: three derived classes constructed as locals, an array of
// base-class pointers holding them (no heap -- the objects outlive the array
// on the stack), and a cross-TU dispatch loop that calls three pure virtual
// methods through that base pointer. No dynamic allocation, so the only
// thing standing between this program and the transpiler is the inheritance
// and virtual-dispatch design itself.
#include <cstdio>

#include "report.hpp"
#include "shape.hpp"

int main(void) {
  Rect r(6, 4);
  Circle c(3);
  RightTriangle t(3, 4);

  Shape *items[3];
  items[0] = &r;
  items[1] = &c;
  items[2] = &t;

  int total = report(items, 3);
  printf("total=%d.%02d\n", total / 100, total % 100);
  return 0;
}
