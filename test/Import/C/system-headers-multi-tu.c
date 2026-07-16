// C99-39 + C99-38: the system-header skip is per translation unit and does
// not disturb multi-TU merging. Both TUs include <stdio.h> (each TU's
// system-header declarations are skipped against its own SourceManager and
// emit no symbols, so per-TU mangling sees nothing new) and both include the
// same project header via -I (imported eagerly in each TU; the struct shape
// dedups to a single struct_def).
// RUN: emitrust-import-c %s %S/Inputs/system-headers-other.c -I%S/Inputs | FileCheck %s

#include <stdio.h>
#include "helper.h"

int point_sum(struct Point p);

int main(void) {
  struct Point p;
  p.x = 1;
  p.y = 2;
  printf("%d\n", point_sum(p));
  return 0;
}

// Exactly one struct_def survives the two imports of helper.h, and nothing
// from <stdio.h> leaks into the module from either TU.
// CHECK-NOT: @fopen
// CHECK: emitrust.struct_def @Point
// CHECK-NOT: emitrust.struct_def
// CHECK-DAG: func.func @point_sum
// CHECK-DAG: func.func @c_main
// CHECK-NOT: @fopen
// CHECK-NOT: @fprintf
