// W3.1 multi-TU gate oracle (G7 NEGATIVE): unlike
// multi-tu-gate-g7-fnptr-devirt-skip.c (a `const`, never-reassignable
// target the gate over-conservatively excludes today), `fp` here is
// NON-const and the companion TU genuinely reassigns it to a different
// function. Even after W3.2 decides fnPtrAliases from a whole-program
// view of every TU's writes, this must stay a real `!emitrust.fn_ptr`
// value with an indirect call — devirtualizing it would silently freeze
// the target at import time and break the reassignment's observable
// behavior. This test pins TODAY's already-correct non-devirtualized
// shape, which must survive unchanged through W3.2.
// RUN: emitrust-import-c %s %S/Inputs/multi-tu-gate-g7-fnptr-devirt-negative-other.c | FileCheck %s

int add(int a, int b) { return a + b; }

int (*fp)(int, int) = &add;

int use_fp(void) { return fp(2, 3); }

int main(void) { return use_fp(); }

// CHECK: emitrust.global @fp <#emitrust.opaque<"Some(add)">> : !emitrust.fn_ptr<(i32, i32) -> i32>
// CHECK-LABEL: func.func @use_fp
// CHECK: emitrust.global_load @fp
// CHECK: emitrust.call_indirect %{{.*}}(%{{.*}}, %{{.*}}) : (!emitrust.fn_ptr<(i32, i32) -> i32>, i32, i32) -> i32
