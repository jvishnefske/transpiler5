// FR-152: the FENCE on the escaping-loop-place hoist, and the reason it is a
// fence rather than a special case.
//
// The fix for FR-152 moves an `emitrust.variable` place that is declared in a
// loop body and escapes the loop OUT of the loop, so the loop-carried lvalue
// becomes invariant and canonicalize can drop it. For a scalar/struct/enum
// that is a pure spelling change. For a type with a destructor it is NOT: the
// place stops being recreated per iteration, so every `Drop` but the last
// disappears. Measured on exactly this program before the fence existed:
//
//   native: ctor 1 body 1 dtor 1 ctor 2 body 2 dtor 2 ctor 3 body 3 dtor 3 3
//   rust:   ctor 1 body 1        ctor 2 body 2        ctor 3 body 3 dtor 3 3
//
// Two `dtor` lines silently deleted -- a compile-clean miscompile that no
// `cargo build` can see. The shape is a hard failure today, so admitting it
// would have been a NEW miscompile introduced by a bug fix.
//
// Rejection is a feature: the fenced shape must be a LOCATED `unsupported:`
// error naming the local, at the DECLARATION, and never a silent skip that
// falls through to the useless `failed to legalize operation 'scf.while'` the
// pipeline would otherwise report three stages later.
// RUN: not emitrust-cc --emit=rust %s -o - 2>&1 | FileCheck %s

#include <cstdio>

struct R {
  int id;
  R(int i) : id(i) { printf("ctor %d\n", id); }
  ~R() { printf("dtor %d\n", id); }
};

int f(int n) {
  for (;;) {
    // CHECK: loop-escaping-place-drop-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: error: unsupported: local 'r' has a destructor, is declared inside a loop body, and escapes the loop
    R r(n);
    printf("body %d\n", r.id);
    n++;
    if (n > 3)
      return r.id;
  }
}

// The note says WHY, so the wording is pinned too.
// CHECK: note: moving the declaration out of the loop would delete every per-iteration drop but the last

// The legalization failure the fence replaces must NOT come back: if it did,
// the diagnostic would be located at the loop rather than the declaration and
// would name an MLIR op instead of the user's variable.
// CHECK-NOT: failed to legalize operation 'scf.while'

int main() {
  printf("%d\n", f(1));
  return 0;
}
