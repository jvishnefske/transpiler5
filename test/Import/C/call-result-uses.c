// RUN: emitrust-import-c %s | FileCheck %s

// CTS 00204 companions: two uses of a struct-returning call's result
// that previously misfired.
//
// (1) printf("%d\n", f().m) — member access on a call result inside a
// printf argument list used to trip the "printf return value must be
// unused" guard (the guard misattributed the inner call). The call
// result materializes into a temporary place and the member load feeds
// the format helper. The temp mechanism is GREEN's choice; only the
// call -> member -> print chain is pinned.
//
// (2) struct T x = f(); — a local struct declaration initialized from
// a call used to reject as "unsupported: aggregate initializer" even
// though the assignment form `x = f();` imports. Both now import.

int printf(const char *fmt, ...);

struct T {
  int m;
  double d;
};

struct pair {
  double a;
  double b;
};

struct T mk(void) {
  struct T t;
  t.m = 7;
  t.d = 1.5;
  return t;
}

struct pair mkp(void) {
  struct pair p;
  p.a = 12.25;
  p.b = 12.5;
  return p;
}

int main(void) {
  // CHECK-LABEL: func.func @c_main

  printf("%d\n", mk().m);
  // CHECK: call @mk() : () -> !emitrust.struct<"T">
  // CHECK: emitrust.member %{{.*}}["m"]
  // CHECK: emitrust.call_opaque "print!"

  // Two independent calls in one argument list materialize two
  // independent temporaries (the 00204 fr_hfa12().a / fr_hfa12().b
  // shape).
  printf("%.1f %.1f\n", mkp().a, mkp().b);
  // CHECK-DAG: call @mkp() : () -> !emitrust.struct<"pair">
  // CHECK-DAG: call @mkp() : () -> !emitrust.struct<"pair">
  // CHECK-DAG: emitrust.member %{{.*}}["a"]
  // CHECK-DAG: emitrust.member %{{.*}}["b"]
  // CHECK: emitrust.call_opaque "print!"

  // Declaration-with-call-initializer imports like the assignment form.
  struct T x = mk();
  printf("%d\n", x.m);
  // CHECK: call @mk() : () -> !emitrust.struct<"T">
  // CHECK: emitrust.member %{{.*}}["m"]
  // CHECK: emitrust.call_opaque "print!"

  return 0;
  // CHECK: return
}
