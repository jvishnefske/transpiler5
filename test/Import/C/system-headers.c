// C99-39: declarations pulled in from a system header (angle-bracket
// include) are skipped at import time instead of being imported eagerly, so
// including a real <stdio.h> succeeds even though its declarations
// (anonymous structs in bits/types.h, variadic prototypes, FILE, ...) are
// far outside the supported subset. Only main-file symbols appear in the
// module, and printf still lowers by name to `print!`.
// RUN: emitrust-import-c %s | FileCheck %s

#include <stdio.h>

int add(int a, int b) { return a + b; }

int main(void) {
  printf("%d\n", add(2, 3));
  return 0;
}

// Nothing from the header may leak into the module: no struct_defs (FILE
// and its bits/types.h dependencies), no imported libc prototypes.
// CHECK-NOT: emitrust.struct_def
// CHECK-NOT: @fopen
// CHECK-NOT: @fclose
// CHECK-NOT: @fprintf
// CHECK-NOT: @printf

// CHECK-LABEL: func.func @add
// CHECK-LABEL: func.func @c_main() -> i32
// CHECK: emitrust.call_opaque "println!"
// CHECK: return

// CHECK-NOT: emitrust.struct_def
// CHECK-NOT: @fopen
