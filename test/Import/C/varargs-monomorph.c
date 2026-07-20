// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | FileCheck %s --check-prefix=ORIG
// RUN: emitrust-import-c %s | FileCheck %s --check-prefix=DISPATCH

// CTS 00204: a variadic DEFINITION whose body uses va_list in the
// bounded shape (va_start on the local ap, va_arg reads, ap never
// escaping, no va_copy, every call direct) is MONOMORPHIZED per call
// site instead of rejected: each call site gets a specialized clone
// whose parameters are the named parameters followed by that site's
// extra arguments as fixed by-value parameters, in order, and nothing
// else. Inside a clone each va_arg(ap, T) becomes a dispatch over an
// integer consumption cursor selecting among the site's extras of
// static type T; a cursor position with no extra of type T is a
// deterministic panic (the C call would be UB, so the panic is a legal
// refinement). va_start resets the cursor; format strings stay runtime
// values inside the clone. Calls are rewritten to the clones; the
// original variadic symbol is NOT emitted, and clone names carry the
// original name plus a suffix. An in-scope va_list-using definition
// with zero call sites is dead by construction (taking a variadic
// definition's address is rejected — varargs-monomorph-invalid.c) and
// is dropped entirely. Out-of-scope va_list uses keep their rejections
// (varargs-monomorph-invalid.c, varargs-def-invalid.c).

typedef __builtin_va_list va_list;
int printf(const char *fmt, ...);

struct P {
  int a;
  int b;
};

void vp(const char *fmt, ...) {
  const char *s;
  va_list ap;
  __builtin_va_start(ap, fmt);
  for (s = fmt; *s; s++) {
    if (*s == 'i') {
      int v = __builtin_va_arg(ap, int);
      printf("%d ", v);
    } else if (*s == 'p') {
      struct P q = __builtin_va_arg(ap, struct P);
      printf("%d:%d ", q.a, q.b);
    }
  }
  printf("\n");
  __builtin_va_end(ap);
}

// An in-scope va_list-using variadic with no call sites: no clones, no
// symbol.
void unused_vp(int n, ...) {
  va_list ap;
  __builtin_va_start(ap, n);
  int v = __builtin_va_arg(ap, int);
  printf("%d\n", v);
  __builtin_va_end(ap);
}

int main(void) {
  struct P p;
  p.a = 1;
  p.b = 2;
  vp("ip", 3, p);
  vp("pi", p, 4);
  vp("ii", 5, 6);
  return 0;
}

// One clone per call site, distinguished by their extras' types: the
// same symbol cannot carry two function types, so pinning the three
// signatures forces three distinct clones. Clone parameter lists are
// exactly (named params, extras) — no trailing cursor or other
// synthetic parameter.
// CHECK-DAG: func.func @{{[A-Za-z0-9_]*vp[A-Za-z0-9_]+}}(%{{[^:]+}}: !emitrust.{{[^,)]+}}, %{{[^:]+}}: i32, %{{[^:]+}}: !emitrust.struct<"P">)
// CHECK-DAG: func.func @{{[A-Za-z0-9_]*vp[A-Za-z0-9_]+}}(%{{[^:]+}}: !emitrust.{{[^,)]+}}, %{{[^:]+}}: !emitrust.struct<"P">, %{{[^:]+}}: i32)
// CHECK-DAG: func.func @{{[A-Za-z0-9_]*vp[A-Za-z0-9_]+}}(%{{[^:]+}}: !emitrust.{{[^,)]+}}, %{{[^:]+}}: i32, %{{[^:]+}}: i32)

// The three calls in main are rewritten to the clones, extras passed as
// ordinary fixed arguments.
// CHECK-LABEL: func.func @c_main
// CHECK: call @{{[A-Za-z0-9_]*vp[A-Za-z0-9_]+}}(%{{[^,)]+}}, %{{[^,)]+}}, %{{[^,)]+}}) : ({{[^,]+}}, i32, !emitrust.struct<"P">) -> ()
// CHECK: call @{{[A-Za-z0-9_]*vp[A-Za-z0-9_]+}}(%{{[^,)]+}}, %{{[^,)]+}}, %{{[^,)]+}}) : ({{[^,]+}}, !emitrust.struct<"P">, i32) -> ()
// CHECK: call @{{[A-Za-z0-9_]*vp[A-Za-z0-9_]+}}(%{{[^,)]+}}, %{{[^,)]+}}, %{{[^,)]+}}) : ({{[^,]+}}, i32, i32) -> ()
// CHECK: return

// The original variadic symbols never surface: no definition or call of
// plain @vp, and the site-less @unused_vp vanishes entirely.
// ORIG-NOT: func.func @vp(
// ORIG-NOT: call @vp(
// ORIG-NOT: unused_vp

// The va_arg cursor dispatch carries a deterministic panic for the
// impossible positions (e.g. the struct va_arg inside the "ii" clone,
// which has no struct-typed extra at all): some panic-family construct
// must exist in the monomorphized module.
// DISPATCH: {{cf\.assert|call_opaque "assert!|call_opaque "panic!|call_opaque "unreachable!}}
