// RUN: split-file %s %t
// RUN: emitrust-import-c %t/fn-address.c | FileCheck %s
// RUN: not emitrust-import-c %t/dangling-local.c 2>&1 | FileCheck %s --check-prefix=DANGLING
// RUN: not emitrust-import-c %t/mixed-returns.c 2>&1 | FileCheck %s --check-prefix=MIXED

// CTS-P2: a data-pointer return type classifies by its return sites
// (principal-kind inference: each `return` contributes one located
// constraint). The supported kind is a returned function address behind a
// `void *` (or matching) pointer return type, which returns the plain
// `!emitrust.fn_ptr` value (the 00095 shape). Returning a cursor into a
// callee-local region stays rejected at the offending return — the
// dangling case.

//--- fn-address.c
int target(void);
void *addr(void) { return &target; }
void *decay(void) { return target; }
int target(void) { return 3; }
// CHECK-LABEL: func.func @addr() -> !emitrust.fn_ptr<() -> i32>
// CHECK: %[[F:.*]] = emitrust.constant <#emitrust.opaque<"Some(target)">> : !emitrust.fn_ptr<() -> i32>
// CHECK: return %[[F]] : !emitrust.fn_ptr<() -> i32>
// CHECK-LABEL: func.func @decay() -> !emitrust.fn_ptr<() -> i32>
// CHECK: emitrust.constant <#emitrust.opaque<"Some(target)">>

//--- dangling-local.c
// Returning a cursor into a callee-local region would dangle: the
// caller's frame never owned the region.
int *first(void) {
  int a[2];
  a[0] = 1;
  return a;
}
int main(void) { return *first(); }
// DANGLING: dangling-local.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: returned pointer value

//--- mixed-returns.c
// Return sites must agree on one function-pointer signature.
int f1(void) { return 1; }
long f2(long x) { return x; }
void *pick(int c) {
  if (c)
    return &f1;
  return &f2;
}
int main(void) { return 0; }
// MIXED: mixed-returns.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: return sites disagree on the returned function pointer signature
