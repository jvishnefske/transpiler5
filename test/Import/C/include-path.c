// C99-39: a project-local header is resolved through the `-I` include path;
// the aggregate types it declares are imported into the module.
// RUN: emitrust-import-c %s -I%S/Inputs | FileCheck %s
// RUN: emitrust-import-c %s -I %S/Inputs | FileCheck %s

#include "helper.h"

struct Point make_point(int x, int y) {
  struct Point p;
  p.x = x;
  p.y = y;
  return p;
}

enum Color pick(void) { return Green; }

// CHECK-DAG: emitrust.struct_def @Point
// CHECK-DAG: emitrust.enum_def @Color
// CHECK-DAG: func.func @make_point
// CHECK-DAG: func.func @pick
