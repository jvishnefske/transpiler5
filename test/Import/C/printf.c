// RUN: emitrust-import-c %s | FileCheck %s

int printf(const char *fmt, ...);

int main(void) {
  int d = 7;
  long l = 42;
  double f = 2.5;
  printf("d=%d l=%ld f=%f done 100%%\n", d, l, f);
  return 0;
}

// The variadic printf declaration itself is not imported.
// CHECK-NOT: func.func private @printf

// C main is imported as c_main; the printf call becomes an opaque call to
// the print! macro with the translated Rust format string in the args
// attribute (%d -> {}, %ld -> {}, %f -> {} of the f64 routed through the
// __emitrust_fmt_f64 helper, %% -> %, \n kept as a real newline byte,
// printed re-escaped as \0A).
// CHECK-LABEL: func.func @c_main() -> i32
// CHECK: %[[F:.*]] = emitrust.call_opaque "__emitrust_fmt_f64"(%{{.*}}) : (f64) -> !emitrust.opaque<"String">
// CHECK: emitrust.call_opaque "print!"(%{{.*}}, %{{.*}}, %[[F]]) {args = ["d={} l={} f={} done 100%\0A", 0 : index, 1 : index, 2 : index]} : (i32, i64, !emitrust.opaque<"String">) -> ()
// CHECK: return

// The %f helper is emitted once at module level: {:.6} matches C for every
// finite value and for infinities, and the NaN branch matches C's
// "nan"/"-nan" spellings (Rust's {:.6} alone would print "NaN").
// CHECK: emitrust.verbatim "fn __emitrust_fmt_f64(x: f64) -> String
// CHECK-SAME: is_nan
// CHECK-SAME: -nan
// CHECK-SAME: {:.6}
