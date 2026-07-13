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
// attribute (%d -> {}, %ld -> {}, %f -> {:.6}, %% -> %, \n kept as a real
// newline byte, printed re-escaped as \0A).
// CHECK-LABEL: func.func @c_main() -> i32
// CHECK: emitrust.call_opaque "print!"(%{{.*}}, %{{.*}}, %{{.*}}) {args = ["d={} l={} f={:.6} done 100%\0A", 0 : index, 1 : index, 2 : index]} : (i32, i64, f64) -> ()
// CHECK: return
