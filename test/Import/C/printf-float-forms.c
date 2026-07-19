// RUN: emitrust-import-c %s | FileCheck %s

// C99-47: the floating directive family. Bare %f (and its C synonym %lf)
// keeps the __emitrust_fmt_f64 fast path; %F/%e/%E/%g/%G and any %f with
// flags, width, or precision route through the __emitrust_fmt_float
// helper (conv 0=f, 1=e, 2=g; flag bits '-'=1, '0'=2, '+'=4, ' '=8,
// '#'=16, uppercase=32).

int printf(const char *fmt, ...);

int main(void) {
  double x = 1.5;

  // Bare %f/%lf keep the existing fast path.
  printf("%f %lf\n", x, x);
  // CHECK-COUNT-2: emitrust.call_opaque "__emitrust_fmt_f64"(%{{.*}}) : (f64) -> !emitrust.opaque<"String">
  // CHECK: emitrust.call_opaque "print!"(%{{.*}}, %{{.*}}) {args = ["{} {}\0A", 0 : index, 1 : index]}

  // Adjusted %f and the e/g families go through __emitrust_fmt_float.
  printf("%.2f %10.3f %-10f %+f % f %#.0f %08.2f\n", x, x, x, x, x, x, x);
  // CHECK-COUNT-7: emitrust.call_opaque "__emitrust_fmt_float"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (f64, i32, i32, i32, i32) -> !emitrust.opaque<"String">
  printf("%e %E %.0e %+10.3e\n", x, x, x, x);
  // CHECK-COUNT-4: emitrust.call_opaque "__emitrust_fmt_float"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (f64, i32, i32, i32, i32) -> !emitrust.opaque<"String">
  printf("%g %G %#g %.17g %le\n", x, x, x, x, x);
  // CHECK-COUNT-5: emitrust.call_opaque "__emitrust_fmt_float"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (f64, i32, i32, i32, i32) -> !emitrust.opaque<"String">
  printf("%F\n", x);
  // CHECK: emitrust.call_opaque "__emitrust_fmt_float"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (f64, i32, i32, i32, i32) -> !emitrust.opaque<"String">

  return 0;
}

// Both float helpers are emitted once at module level: the bare-%f one
// and the exact C99 f/e/g family (digits extractor, exponent renderer,
// and the main entry).
// CHECK: emitrust.verbatim "fn __emitrust_fmt_f64(x: f64) -> String
// CHECK: emitrust.verbatim "fn __emitrust_fmt_exp(m: &str, ev: i32, alt: bool, upper: bool) -> String
// CHECK-SAME: fn __emitrust_fmt_edigits(mag: f64, prec: usize) -> (String, i32, bool)
// CHECK-SAME: fn __emitrust_fmt_float(x: f64, conv: i32, prec: i32, width: i32,
