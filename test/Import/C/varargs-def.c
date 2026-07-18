// CTS-P9 (00140): a variadic function DEFINITION whose body never touches
// va_list (no va_list declarations, no va_start/va_arg/va_copy) imports as
// its fixed prototype — the named parameters only, the trailing `...`
// dropped from the type. Call sites drop trailing extra arguments when
// every dropped extra is side-effect-free; the dropped extras are never
// imported, so a dropped `&s` must not create a pointer region or borrow
// and a dropped by-value struct must not be loaded. Definitions that DO
// use va_list keep the existing rejection (see varargs-def-invalid.c).
// RUN: emitrust-import-c %s | FileCheck %s

struct S {
  int a;
  int b;
};

int f(int x, ...) {
  return x + 1;
}

int g(struct S v, struct S *p, int n, ...) {
  if (v.a != p->a)
    return 0;
  return p->b + n;
}

int use_f(void) {
  struct S s;
  s.a = 5;
  s.b = 6;
  int r1 = f(1);
  int r2 = f(1, 2, 3);
  int r3 = f(s.a, s, &s);
  return r1 + r2 + r3;
}

int use_g(void) {
  struct S s;
  s.a = 1;
  s.b = 2;
  g(s, &s, 2);
  g(s, &s, 2, 1, s, &s);
  return g(s, &s, 3) - 5;
}

// The va_list-free variadic definitions import with only their named
// parameters.
// CHECK-LABEL: func.func @f
// CHECK-SAME: (%{{[^,)]+}}: i32) -> i32

// CHECK-LABEL: func.func @g
// CHECK-SAME: (%{{[^,)]+}}: !emitrust.struct<"S">, %{{[^,)]+}}: !emitrust.{{(mut_)?}}ref<!emitrust.struct<"S">>, %{{[^,)]+}}: i32) -> i32

// Every call to f lowers to a single-argument call regardless of how many
// effect-free extras the C source passes. use_f takes no address anywhere:
// the dropped `&s` must not create a region or borrow, and the dropped
// by-value struct extra must not be loaded.
// CHECK-LABEL: func.func @use_f
// CHECK-NOT: emitrust.addr_of
// CHECK: call @f(%{{[^,)]+}}) : (i32) -> i32
// CHECK: call @f(%{{[^,)]+}}) : (i32) -> i32
// CHECK-NOT: emitrust.addr_of
// CHECK-NOT: emitrust.load {{.*}} -> !emitrust.struct
// CHECK: call @f(%{{[^,)]+}}) : (i32) -> i32
// CHECK-NOT: emitrust.addr_of
// CHECK: return

// The 00140 shape: statement-position calls with a by-value struct, a
// struct pointer, and an int as the named arguments. Each call carries
// exactly three arguments; the NAMED &s argument borrows once per call and
// the dropped `1, s, &s` extras of the second call add no second borrow.
// CHECK-LABEL: func.func @use_g
// CHECK: emitrust.addr_of
// CHECK: call @g(%{{[^,)]+}}, %{{[^,)]+}}, %{{[^,)]+}}) : ({{.*}}) -> i32
// CHECK: emitrust.addr_of
// CHECK-NOT: emitrust.addr_of
// CHECK: call @g(%{{[^,)]+}}, %{{[^,)]+}}, %{{[^,)]+}}) : ({{.*}}) -> i32
// CHECK: call @g(%{{[^,)]+}}, %{{[^,)]+}}, %{{[^,)]+}}) : ({{.*}}) -> i32
// CHECK: return
