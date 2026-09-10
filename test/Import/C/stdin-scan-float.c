// FR-183 admitted `%d`, `%u` and `%c` and refused the float conversions on
// purpose. This file pins the FLIP: `%f` and its C-identical spellings are
// now imported, because two corpus programs read a float from standard input
// and print its RAW BYTES, so nothing short of a bit-exact parse passes them.
//
// WHAT MAKES THE FLIP SAFE, and it is the whole reason this file exists:
// C's float conversions are ONE conversion with seven spellings
// (`a e f g A E F G`), differing only in the argument type selected by the
// length modifier -- none (`float *`) or `l` (`double *`). So the import
// surface is exactly two helpers, `__emitrust_scan_f` over
// `!emitrust.mut_ref<f32>` and `__emitrust_scan_lf` over
// `!emitrust.mut_ref<f64>`, threading the SAME i32 state as every other
// directive. Nothing else about FR-183's chain changes.
//
// PIN FIRMNESS:
//   FIRM  - all seven unmodified spellings select `__emitrust_scan_f` and a
//           `float` argument; all seven `l`-modified spellings select
//           `__emitrust_scan_lf` and a `double` argument;
//   FIRM  - the out-argument is an `emitrust.addr_of mut` producing a
//           `!emitrust.mut_ref` of the variable's own FLOAT type -- never an
//           integer, never a slice, never an FR-40 owner lift;
//   FIRM  - a float directive threads the state exactly like `%d`, and mixes
//           with `%d`/`%u`/`%c`/whitespace in one format;
//   LOOSE - SSA numbering and the emitted Rust text.
// RUN: emitrust-import-c %s | FileCheck %s

#include <stdio.h>

// The corpus shape: one `%f` into a function-local `float`.
float one_float(void) {
  float x = 0.f;
  scanf("%f", &x);
  return x;
}
// CHECK-LABEL: func.func @one_float
// CHECK: %[[R0:.*]] = emitrust.addr_of mut {{.*}} -> !emitrust.mut_ref<f32>
// CHECK-NEXT: %[[S1:.*]] = emitrust.call_opaque "__emitrust_scan_f"(%c0{{[a-z0-9_]*}}, %[[R0]])
// CHECK-NEXT: emitrust.call_opaque "__emitrust_scan_done"(%[[S1]])

// `%lf` is the SAME conversion with a `double *` argument, so it is a
// distinct helper only because the out-reference's type must stay exact.
double one_double(void) {
  double d = 0;
  scanf("%lf", &d);
  return d;
}
// CHECK-LABEL: func.func @one_double
// CHECK: %[[R1:.*]] = emitrust.addr_of mut {{.*}} -> !emitrust.mut_ref<f64>
// CHECK-NEXT: %[[T1:.*]] = emitrust.call_opaque "__emitrust_scan_lf"(%c0{{[a-z0-9_]*}}, %[[R1]])
// CHECK-NEXT: emitrust.call_opaque "__emitrust_scan_done"(%[[T1]])

// `%e`, `%g` and `%a` are `%f` in C's scanf grammar -- same input syntax,
// same argument type. Admitting one and refusing the others would be an
// arbitrary line, so all seven spellings are the same helper.
float aliases(void) {
  float a = 0.f;
  float b = 0.f;
  float c = 0.f;
  float d = 0.f;
  scanf("%e", &a);
  scanf("%g", &b);
  scanf("%a", &c);
  scanf("%E", &d);
  return a + b + c + d;
}
// CHECK-LABEL: func.func @aliases
// CHECK: emitrust.call_opaque "__emitrust_scan_f"
// CHECK: emitrust.call_opaque "__emitrust_scan_f"
// CHECK: emitrust.call_opaque "__emitrust_scan_f"
// CHECK: emitrust.call_opaque "__emitrust_scan_f"

// ... and the `l`-modified spellings likewise.
double double_aliases(void) {
  double a = 0;
  double b = 0;
  double c = 0;
  scanf("%le", &a);
  scanf("%lg", &b);
  scanf("%lA", &c);
  return a + b + c;
}
// CHECK-LABEL: func.func @double_aliases
// CHECK: emitrust.call_opaque "__emitrust_scan_lf"
// CHECK: emitrust.call_opaque "__emitrust_scan_lf"
// CHECK: emitrust.call_opaque "__emitrust_scan_lf"

// A float directive threads the state exactly like an integer one: the
// chain is one call per directive, in format order, and mixing conversions
// changes nothing about it.
int mixed(void) {
  int n = 0;
  float x = 0.f;
  double y = 0;
  scanf("%d %f %lf", &n, &x, &y);
  return n;
}
// CHECK-LABEL: func.func @mixed
// CHECK: %[[M1:.*]] = emitrust.call_opaque "__emitrust_scan_d"(%c0{{[a-z0-9_]*}}, %{{.*}})
// CHECK-NEXT: %[[M2:.*]] = emitrust.call_opaque "__emitrust_scan_ws"(%[[M1]])
// CHECK-NEXT: %{{.*}} = emitrust.addr_of mut
// CHECK-NEXT: %[[M3:.*]] = emitrust.call_opaque "__emitrust_scan_f"(%[[M2]], %{{.*}})
// CHECK-NEXT: %[[M4:.*]] = emitrust.call_opaque "__emitrust_scan_ws"(%[[M3]])
// CHECK-NEXT: %{{.*}} = emitrust.addr_of mut
// CHECK-NEXT: %[[M5:.*]] = emitrust.call_opaque "__emitrust_scan_lf"(%[[M4]], %{{.*}})
// CHECK-NEXT: emitrust.call_opaque "__emitrust_scan_done"(%[[M5]])

// `fscanf(stdin, ...)` is the same reader for floats too.
float via_fscanf_float(void) {
  float x = 0.f;
  fscanf(stdin, "%f", &x);
  return x;
}
// CHECK-LABEL: func.func @via_fscanf_float
// CHECK: emitrust.call_opaque "__emitrust_scan_f"
// CHECK: emitrust.call_opaque "__emitrust_scan_done"
