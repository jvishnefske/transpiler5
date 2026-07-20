// RUN: emitrust-import-c %s | FileCheck %s

// CTS 00204 / C99-8 policy REVISION: `long double` now maps to f64, the
// same type as `double` (UB-refinement: C requires long double to be at
// least as wide as double; every f64-exact value round-trips, and the
// 00204 shapes perform no long-double arithmetic — only f64-exact
// literals widened, copied, and printed). Consequences pinned here:
//  - long double locals, parameters, returns, and struct members all
//    type as f64; no arith.extf/arith.truncf is emitted for the
//    double <-> long double conversions (they are identities under the
//    mapping).
//  - L-suffixed literals (decimal and hex) import as f64 constants of
//    the same value.
//  - printf's L length modifier on the floating conversions (f/F/e/E/
//    g/G) lowers exactly like the unmodified twin: bare %Lf keeps the
//    __emitrust_fmt_f64 fast path; adjusted forms and the e/g families
//    route through __emitrust_fmt_float.
// Still-out long-double shapes are pinned in long-double-f64-invalid.c.

int printf(const char *fmt, ...);

// An L-suffixed literal initializing a long double local, then narrowed
// to double: both types are f64, the literal is an f64 constant, and no
// truncation is emitted.
double narrow(void) {
  long double x = 2.5L;
  double d = x;
  return d;
}
// CHECK-LABEL: func.func @narrow
// CHECK: memref.alloca() : memref<f64>
// CHECK: arith.constant 2.500000e+00 : f64
// CHECK-NOT: arith.truncf
// CHECK: return %{{.*}} : f64

// Widening double -> long double is the identity: parameter f64 in,
// result f64 out, no extension.
long double widen(double d) {
  long double x = d;
  return x;
}
// CHECK-LABEL: func.func @widen
// CHECK-SAME: (%{{[^,)]+}}: f64) -> f64
// CHECK-NOT: arith.extf

// Long double parameters and returns are f64 like double's.
long double pick(long double a, long double b) {
  return b;
}
// CHECK-LABEL: func.func @pick
// CHECK-SAME: (%{{[^,)]+}}: f64, %{{[^,)]+}}: f64) -> f64

// Struct members of type long double are f64 fields (the 00204 hfa3x
// shape).
struct hfa2 {
  long double a;
  long double b;
};
// CHECK: emitrust.struct_def @hfa2 ["a", "b"] [f64, f64]

int main(void) {
  struct hfa2 h;
  h.a = 31.5;
  h.b = 0x1.8p3L;
  long double x = pick(h.a, h.b);

  // Bare %Lf keeps the %f fast path.
  printf("%Lf\n", x);
  // CHECK-LABEL: func.func @c_main
  // CHECK: call @pick(%{{[^,)]+}}, %{{[^,)]+}}) : (f64, f64) -> f64
  // CHECK: emitrust.call_opaque "__emitrust_fmt_f64"(%{{.*}}) : (f64) -> !emitrust.opaque<"String">

  // Precision/width-adjusted %Lf routes through the same helper as the
  // %f twin.
  printf("%.1Lf %12.3Lf\n", h.a, h.b);
  // CHECK-COUNT-2: emitrust.call_opaque "__emitrust_fmt_float"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (f64, i32, i32, i32, i32) -> !emitrust.opaque<"String">

  // The rest of the floating family accepts L identically: %LF, %Le,
  // %LG behave like %F, %e, %G.
  printf("%LF %Le %LG\n", x, x, x);
  // CHECK-COUNT-3: emitrust.call_opaque "__emitrust_fmt_float"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (f64, i32, i32, i32, i32) -> !emitrust.opaque<"String">

  double nd = narrow();
  printf("%.1f\n", nd);
  return 0;
}
