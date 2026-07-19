// RUN: split-file %s %t
// RUN: emitrust-import-c %t/subset.c | FileCheck %s
// RUN: emitrust-import-c %t/user-abs.c | FileCheck %s --check-prefix=USERABS
// RUN: emitrust-import-c %t/user-atoi.c | FileCheck %s --check-prefix=USERATOI

// C99-48: the curated stdlib/string/math additions to the hosted libc
// subset. Every intercept is by name and definition-less only: abs/labs
// lower to wrapping_abs (the INT_MIN wrap deterministically refines C's
// UB), atoi parses the argument's char region through the
// `__emitrust_atoi` helper with C's exact semantics, statement-position
// exit lowers to std::process::exit, memmove shares memcpy's lowering
// (distinct regions never overlap; the same-object shape is copy_within,
// which IS memmove), and the IEEE-exact fabs/sqrt/floor/ceil lower to
// the matching f64 methods. A user-defined function of any curated name
// stays an ordinary imported call.

//--- subset.c
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  char buf[8];
  char num[8] = " -42x";

  // CHECK-LABEL: func.func @c_main

  // memmove between distinct regions takes the memcpy helper (distinct
  // char regions can never overlap).
  strcpy(buf, "abcdef");
  memmove(buf, "XY", 2);
  // CHECK: emitrust.call_opaque "__emitrust_memcpy"(%{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<i8>>, !emitrust.ref<!emitrust.slice<i8>>, i64) -> ()

  // Same-object memmove borrows the whole array once and copies through
  // copy_within (exactly memmove's overlap-correct semantics).
  memmove(&buf[1], buf, 3);
  // CHECK: emitrust.call_opaque "__emitrust_memcpy_within"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<i8>>, i64, i64, i64) -> ()

  // abs/labs onto wrapping_abs at the C types' widths.
  int a = abs(-5);
  // CHECK: emitrust.call_opaque "i32::wrapping_abs"(%{{.*}}) : (i32) -> i32
  long b = labs(-6L);
  // CHECK: emitrust.call_opaque "i64::wrapping_abs"(%{{.*}}) : (i64) -> i64

  // atoi over a char-array region: shared byte slice into the helper.
  int n = atoi(num);
  // CHECK: emitrust.call_opaque "__emitrust_atoi"(%{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>) -> i32
  // atoi of a string literal reads the literal's read-only backing.
  int m = atoi("17");
  // CHECK: emitrust.call_opaque "__emitrust_atoi"(%{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>) -> i32

  // The IEEE-exact <math.h> functions onto their f64 methods.
  double d = fabs(-2.5) + sqrt(2.0) + floor(1.7) + ceil(1.2);
  // CHECK: emitrust.call_opaque "f64::abs"(%{{.*}}) : (f64) -> f64
  // CHECK: emitrust.call_opaque "f64::sqrt"(%{{.*}}) : (f64) -> f64
  // CHECK: emitrust.call_opaque "f64::floor"(%{{.*}}) : (f64) -> f64
  // CHECK: emitrust.call_opaque "f64::ceil"(%{{.*}}) : (f64) -> f64

  printf("%s %d %ld %d %d %f\n", buf, a, b, n, m, d);

  // Statement-position exit terminates with C's status semantics.
  exit(3);
  // CHECK: emitrust.call_opaque "std::process::exit"(%{{.*}}) : (i32) -> ()
}
// The atoi helper is emitted once per module, after all imported items.
// CHECK: emitrust.verbatim "fn __emitrust_atoi(s: &[i8]) -> i32

//--- user-abs.c
// A project-supplied abs is NOT intercepted: the call imports as an
// ordinary func.call to the user definition.
int abs(int x) { return x < 0 ? -x : x; }
int main(void) {
  return abs(-3) - 3;
}
// USERABS-LABEL: func.func @abs(
// USERABS: func.func @c_main
// USERABS: call @abs(%{{.*}}) : (i32) -> i32
// USERABS-NOT: wrapping_abs

//--- user-atoi.c
// A project-supplied atoi is NOT intercepted either (any signature; the
// hosted parse applies only when no definition exists).
int atoi(int x) { return x + 1; }
int main(void) {
  return atoi(41) - 42;
}
// USERATOI-LABEL: func.func @atoi(
// USERATOI: func.func @c_main
// USERATOI: call @atoi(%{{.*}}) : (i32) -> i32
// USERATOI-NOT: __emitrust_atoi
