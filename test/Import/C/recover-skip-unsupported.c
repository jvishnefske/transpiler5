// RUN: emitrust-cc --recover --emit=import %s -o - 2>%t.err | FileCheck %s
// RUN: FileCheck %s --check-prefix=DIAG --input-file=%t.err

// FR-42 recoverable import: one supported function and one unsupported
// function in the same translation unit. With --recover the module still
// contains the supported function, the unsupported one is replaced by a stub
// carrying its real mapped signature, the rejection is a WARNING (not an
// error), and the driver exits 0 — so this RUN line has no `not`.
//
// The companion negative test (recover-off-by-default.c) pins that the very
// same input still hard-errors without the flag.

int supported(int x) { return x + 1; }

// Rejected for its volatile local (C99-7). The SIGNATURE (i32) -> i32 maps
// perfectly, so this is the stubbable case.
int unsupported(int x) {
  volatile int v = x;
  return v;
}

int main(void) { return supported(1) + unsupported(2); }

// The supported function keeps its real body.
// CHECK-LABEL: func.func @supported
// CHECK: arith.addi

// The rejected function survives as a stub with the identical signature and
// an `unimplemented!()` body carrying the verbatim rejection text. No part
// of the half-built real body (the volatile local's cell, its store) is left
// behind.
// CHECK-LABEL: func.func @unsupported(
// CHECK-SAME: i32) -> i32 {
// CHECK-NEXT: emitrust.call_opaque "unimplemented!"()
// CHECK-SAME: unsupported: volatile-qualified type
// CHECK-NEXT: return
// CHECK-NEXT: }
// CHECK-NOT: memref.alloca

// The caller is untouched and still calls the stub.
// CHECK-LABEL: func.func @c_main
// CHECK: call @supported
// CHECK: call @unsupported

// DIAG: recover-skip-unsupported.c:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: volatile-qualified type (recovered: emitted an unimplemented!() stub with the mapped signature)
// DIAG-NOT: error:
// DIAG: recovered 1 rejected top-level item:
// DIAG: stubbed 'unsupported' [other] unsupported: volatile-qualified type
// DIAG: blocker tabulation (recovered items by tag):
// DIAG: other 1
