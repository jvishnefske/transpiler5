// REQUIRES: cargo
// FR-62 slice 4 (stage A) differential test, DEMOTION RULE 3: the plan
// roles `addall` as TOTAL's arm, but the importer monomorphizes the
// variadic definition per call site, so no `emitrust.func @addall` exists
// in the IR — a lift would delete a global the monomorph still reads. The
// pins: the demotion warning names the actor and the
// no-imported-function reason, the crate still builds in today's
// thread-local form, and the byte-diff against the clang native is green
// because the module is untouched (demotion is not an error).
// RUN: emitrust-cc --actor-lift --emit=crate %s -o %t.crate --build 2> %t.err
// RUN: FileCheck %s < %t.err
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/actor_lift_variadic > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// CHECK: warning: actor plan: demoted TOTAL: plan function 'addall' has no imported function (variadic monomorphization or a recovered item)

int printf(const char *, ...);
typedef __builtin_va_list va_list;
int total;
int addall(int n, ...) {
  va_list ap;
  __builtin_va_start(ap, n);
  for (int i = 0; i < n; ++i) total += __builtin_va_arg(ap, int);
  __builtin_va_end(ap);
  return total;
}
int main(void) { printf("t=%d\n", addall(2, 3, 4)); return 0; }
