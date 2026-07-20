// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/conflict.c 2>&1 | FileCheck %s --check-prefix=CONFLICT
// RUN: not emitrust-import-c %t/member.c 2>&1 | FileCheck %s --check-prefix=MEMBER
// RUN: not emitrust-import-c %t/bound.c 2>&1 | FileCheck %s --check-prefix=BOUND
// RUN: not emitrust-import-c %t/unrefined.c 2>&1 | FileCheck %s --check-prefix=UNREFINED

// FR-29 / CTS 00209: the pinned rejections around K&R callsite-prototype
// inference (positives in fnptr-noproto-infer.c). Conflicting inferences
// across call sites for one decl are a NEW located diagnostic at the
// second, disagreeing site. A no-proto call whose callee is NOT
// traceable to a single ParmVarDecl/VarDecl (here: a struct member)
// RETAINS the existing no-prototype wording. Binding a real function
// with an incompatible definition to an inferred pointer keeps the
// existing signature-mismatch wording (resolveFunctionPointerDecl). And
// passing an unrefined fn_ptr<() -> i32> value into a refined-signature
// position keeps the existing call-argument-mismatch wording.

//--- conflict.c
int conflict(int (*fp)(), int i) {
  int a = (*fp)(i);
  int b = (*fp)(i, i);
  return a + b;
}
// CONFLICT: conflict.c:3:{{[0-9]+}}: error: unsupported: conflicting inferred prototypes for function pointer 'fp'

//--- member.c
struct S { int (*op)(); };
int member_call(void) {
  struct S v;
  v.op = 0;
  return v.op(7);
}
// MEMBER: member.c:5:{{[0-9]+}}: error: unsupported: call with arguments through a function pointer without a prototype

//--- bound.c
int two(int a, int b) { return a + b; }
int bound_mismatch(void) {
  int (*np)() = two;
  return np(1);
}
// BOUND: error: unsupported: function 'two' does not match the function pointer signature

//--- unrefined.c
int zero(void) { return 0; }
int taker(int (*fp)(), int i) { return (*fp)(i); }
int pass_unrefined(void) {
  int (*np)() = zero;
  return taker(np, 3);
}
// UNREFINED: unrefined.c:5:{{[0-9]+}}: error: unsupported: call argument type mismatch
