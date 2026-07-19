// RUN: emitrust-import-c %s | FileCheck %s

// C99-18: inline functions. The `inline` specifier is a semantic no-op
// for the transpiler — every inline definition imports as an ordinary
// function. The tricky C99 linkage case is the plain `inline` definition
// without `extern` (C99 6.7.4p7: an inline definition that provides no
// external definition of the symbol); clang still hands the importer the
// full body, and in the merged whole-program module a single ordinary
// definition is exactly the right shape. `static inline` follows the
// ordinary internal-linkage path (bare name in a single-TU import), and
// `extern inline` — the C99 spelling that DOES provide the external
// definition — imports identically.

/* Plain C99 inline: inline definition without an external one. */
inline int addi(int a, int b) {
  return a + b;
}

/* Internal linkage: static inline is an ordinary file-static. */
static inline int muli(int a, int b) {
  return a * b;
}

/* extern inline: the external definition, same import. */
extern inline int subi(int a, int b) {
  return a - b;
}

int drive(void) {
  return subi(addi(muli(3, 4), 5), 2);
}

// All three import as ordinary definitions with their bodies; the inline
// specifier leaves no trace. The static inline keeps its bare name in a
// single-TU import (the per-TU tag is empty).
// CHECK-LABEL: func.func @addi
// CHECK-SAME: (%{{.*}}: i32, %{{.*}}: i32) -> i32
// CHECK: arith.addi
// CHECK: return

// CHECK-LABEL: func.func @muli
// CHECK: arith.muli
// CHECK: return

// CHECK-LABEL: func.func @subi
// CHECK: arith.subi
// CHECK: return

// Calls resolve by name like any other direct call.
// CHECK-LABEL: func.func @drive
// CHECK: call @muli
// CHECK: call @addi
// CHECK: call @subi
