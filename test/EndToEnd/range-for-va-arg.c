// FR-61f-9, a CRASH REGRESSION from the FR-61f-6 aggregate widening, and the
// exact "latent shield removed" shape FR-61f-4 warned about.
//
// `va_list` is `struct __va_list_tag[1]` on the SysV ABI -- a ConstantArrayType
// -- so once the body whitelist admitted an array of ANY element type, a body
// walking a `va_list` became range-eligible. But `va_arg` lowers to control
// flow of its own (the register-save-area walk), which cannot live in the
// single-block `emitrust.for` region: the result was a SEGFAULT inside MLIR's
// `verifyNSuccessors`, not a located diagnostic.
//
// It was latent at the commit that introduced it. The one corpus instance,
// test/EndToEnd/actor-lift-variadic.c, accumulates into a GLOBAL, and globals
// were still rejected earlier in the clause chain -- so the whole suite stayed
// green over a compiler that crashed on this file, which differs from that one
// only in accumulating into a LOCAL. Spiking the globals remainder removed the
// shield and the crash surfaced immediately.
//
// Two fences, deliberately both: `blocksRangeForLift` refuses `VAArgExpr`
// (a statement about the EXPRESSION -- the hazard is the walk), and the body
// whitelist refuses the `va_list` TYPE (a statement about the VARIABLE, which
// also covers `va_start`/`va_end` naming `ap`).
//
// REQUIRES: cargo
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/range_for_va_arg > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s

int printf(const char *, ...);
typedef __builtin_va_list va_list;

// The crashing shape: a LOCAL accumulator, so nothing else in the body
// rejects first and the `va_list` fence is the only thing standing.
// CHECK-LABEL: fn addall
// CHECK-NOT:     for {{.*}} in
int addall(int n, ...) {
  va_list ap;
  int total = 0;
  __builtin_va_start(ap, n);
  for (int i = 0; i < n; ++i)
    total += __builtin_va_arg(ap, int);
  __builtin_va_end(ap);
  return total;
}

// An ordinary array in the body still lifts: the fence is on `va_list`, not
// on arrays, so FR-61f-6 is not walked back.
// CHECK-LABEL: fn plain_array
// CHECK:         for {{i|_i}} in 0i32..
int plain_array(int n) {
  int a[8];
  int s = 0;
  for (int i = 0; i < 8; i++)
    a[i] = i * 3;
  for (int i = 0; i < n; i++)
    s += a[i & 7];
  return s;
}

int main(void) {
  printf("t=%d\n", addall(3, 4, 5, 6));
  printf("p=%d\n", plain_array(7));
  return 0;
}
