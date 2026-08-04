// CTS-F (00207): an UNREFERENCED variable-length array local whose size
// expression is side-effect-free is ELIDED at import: the object never
// materializes (no array-typed emitrust.variable reaches the IR), the
// size expression is dropped, and the rest of the function imports
// untouched. Referenced VLAs — and VLAs whose size expression has side
// effects, even when the object is unused — keep the verbatim rejection
// (see vla-dead-elision-invalid.c).
// RUN: emitrust-import-c %s | FileCheck %s

extern int printf(const char *, ...);

// The 00207 f1 shape verbatim: the dead VLA disappears while the
// if(0)-label backward goto, the argc-- flow, and the print! survive
// unchanged.
void f1(int argc)
{
  char test[argc];
  if(0)
  label:
    printf("boom!\n");
  if(argc-- == 0)
    return;
  goto label;
}
// CHECK-LABEL: func.func @f1
// CHECK-NOT: !emitrust.array
// CHECK: emitrust.call_opaque "println!"() {args = ["boom!"]}
// CHECK-NOT: !emitrust.array

// A dead VLA with a compound — but still side-effect-free — size
// expression is elided the same way; the remaining body is untouched.
int dead_compound(int n) {
  char scratch[(n + 1) * 2];
  return n + 7;
}
// CHECK-LABEL: func.func @dead_compound
// CHECK-NOT: !emitrust.array
// CHECK: arith.addi
// CHECK: return
// CHECK-NOT: !emitrust.array
