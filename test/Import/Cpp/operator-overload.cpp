// RUN: emitrust-import-c %s | FileCheck %s

// W2.25: operator overloading, the admitted subset — FREE by-value
// operators import as ordinary functions under SYNTHESIZED identifier
// spellings (`operator+` -> `op_add`) with FR-114's per-parameter suffixes,
// and MEMBER by-value flat-call operators import as ordinary methods
// (`operator==` -> `S_op_eq`) dispatched through the same method-call
// machinery as a named method. This file pins the POSITIVE half; every
// still-fenced shape keeps its located rejection in
// operator-overload-invalid.cpp / free-operator-invalid.cpp.
//
// What is pinned, and why each pin is load-bearing:
//
// 1. FREE SUFFIX SPLIT (FR-119's recorded raytracing blocker): THREE free
//    `operator*` overloads over one class all synthesize the base `op_mul`
//    and would map to ONE symbol without FR-114's suffixes; the per-param
//    codes split them (op_mul_rv_i32 / op_mul_i32_rv / op_mul_rv_rv), and
//    each call site resolves to the intended overload. `operator+` is the
//    sole owner of `op_add` and keeps the bare synthesized name (the
//    FR-114 zero-churn rule applies to synthesized spellings too).
//
// 2. MEMBER SYNTHESIS: `S_op_eq` / `S_op_ne` / `S_op_index` /
//    `S_op_call_*` / `S_op_sub` — including an OUT-OF-LINE definition
//    (`S::operator==`, which used to reject at the def under FR-117), the
//    `!(*this == o)` receiver-deref body spelling, a unary member operator
//    (zero-parameter `operator-`), and a genuine member-operator OVERLOAD
//    SET (`operator()(int)` vs `operator()(int,int)`) suffixed by the
//    frozen W2.2 member codes.
//
// 3. SUFFIX-NUMBERING STABILITY (the wave's constraint 8): class `M` mixes
//    an identifier overload set (`get`) with an admitted operator. The
//    overload counter now assigns operators real base names, so this pins
//    that the IDENTIFIER siblings' suffixes are byte-identical to what
//    they were when the operator was an empty-named omission (M_get_i /
//    M_get_b, methods.cpp's frozen codes), and that a synthesized base
//    (`op_lt`) never joins an identifier set.
//
// 4. CALL ROUTING: operator syntax (`a + b`, `s1 == s2`, `s1[4]`, `s1(3)`,
//    `-s1`, `!c`) and the EXPLICIT member spelling (`s1.operator==(s2)`)
//    both resolve through the one synthesized symbol; member dispatch
//    carries the `emitrust.method_call` marker exactly like a named
//    method call.

extern "C" int printf(const char *, ...);

// -- free operators: the vec3 shape ---------------------------------------
struct V {
  int x;
  int y;
};
V operator+(const V &a, const V &b) { V r; r.x = a.x + b.x; r.y = a.y + b.y; return r; }
V operator*(const V &a, int t) { V r; r.x = a.x * t; r.y = a.y * t; return r; }
V operator*(int t, const V &a) { V r; r.x = t * a.x; r.y = t * a.y; return r; }
V operator*(const V &a, const V &b) { V r; r.x = a.x * b.x; r.y = a.y * b.y; return r; }
bool operator!(const V &a) { return a.x == 0 && a.y == 0; }

// CHECK-LABEL: func.func @op_add(
// CHECK-SAME: !emitrust.ref<!emitrust.struct<"V">>, %{{.*}}: !emitrust.ref<!emitrust.struct<"V">>) -> !emitrust.struct<"V">
// CHECK-LABEL: func.func @op_mul_rv_i32(
// CHECK-SAME: !emitrust.ref<!emitrust.struct<"V">>, %{{.*}}: i32) -> !emitrust.struct<"V">
// CHECK-LABEL: func.func @op_mul_i32_rv(
// CHECK-SAME: i32, %{{.*}}: !emitrust.ref<!emitrust.struct<"V">>) -> !emitrust.struct<"V">
// CHECK-LABEL: func.func @op_mul_rv_rv(
// CHECK-LABEL: func.func @op_not(
// CHECK-SAME: -> i1

// -- member operators, out-of-line def, overload set, unary ----------------
struct S {
  int a;
  bool operator==(const S &o) const;
  bool operator!=(const S &o) const { return !(*this == o); }
  int operator[](int i) const { return a + i; }
  int operator()(int i) const { return a * i; }
  int operator()(int i, int j) const { return a * i + j; }
  S operator-() const { S r; r.a = -a; return r; }
};
bool S::operator==(const S &o) const { return a == o.a; }

// CHECK-LABEL: func.func @S_op_ne(
// CHECK-SAME: emitrust.method_of = "S"
// CHECK: call @S_op_eq(
// CHECK-LABEL: func.func @S_op_index(
// CHECK-SAME: i32) -> i32
// CHECK-LABEL: func.func @S_op_call_i(
// CHECK-SAME: i32) -> i32
// CHECK-LABEL: func.func @S_op_call_ii(
// CHECK-SAME: i32, %{{.*}}: i32) -> i32
// CHECK-LABEL: func.func @S_op_sub(
// CHECK-SAME: !emitrust.ref<!emitrust.struct<"S">>) -> !emitrust.struct<"S">
// CHECK-LABEL: func.func @S_op_eq(
// CHECK-SAME: emitrust.method_of = "S"

// -- identifier overload set beside an operator: suffixes are stable -------
struct M {
  int t;
  int get(int x) const { return t + x; }
  int get(bool f) const { return f ? t : 0; }
  bool operator<(const M &o) const { return t < o.t; }
};

// CHECK-LABEL: func.func @M_get_i(
// CHECK-LABEL: func.func @M_get_b(
// CHECK-LABEL: func.func @M_op_lt(

int use(int n) {
  V u; u.x = n; u.y = 2;
  V v; v.x = 3; v.y = n;
  V c = u + v + (u * v);
  V d = (u * n) + (n * v);
  int nz = (!c) ? 1 : 0;
  S s1; s1.a = n;
  S s2; s2.a = n + 1;
  int e1 = (s1 == s2) ? 1 : 0;
  int e2 = (s1 != s2) ? 1 : 0;
  int e3 = s1[4];
  int e4 = s1(3);
  int e5 = s1(3, 9);
  int e6 = s1.operator==(s2) ? 5 : 6;
  S neg = -s1;
  M m1; m1.t = n;
  M m2; m2.t = n + 2;
  int e7 = (m1 < m2) ? 1 : 0;
  int e8 = m1.get(3) + m2.get(true);
  return c.x + d.y + nz + e1 + e2 + e3 + e4 + e5 + e6 + e7 + e8 + neg.a;
}

// CHECK-LABEL: func.func @use_(
// Chained free operators: u + v + (u * v) — the inner results feed the
// outer call as borrowed temporaries.
// CHECK: call @op_mul_rv_rv(
// CHECK: call @op_add(
// CHECK: call @op_mul_rv_i32(
// CHECK: call @op_mul_i32_rv(
// CHECK: call @op_add(
// CHECK: call @op_not(
// Member dispatch carries the method-call marker; the explicit spelling
// `s1.operator==(s2)` resolves to the SAME symbol as `s1 == s2`.
// CHECK: call @S_op_eq({{.*}}) {emitrust.method_call}
// CHECK: call @S_op_ne({{.*}}) {emitrust.method_call}
// CHECK: call @S_op_index({{.*}}) {emitrust.method_call}
// CHECK: call @S_op_call_i({{.*}}) {emitrust.method_call}
// CHECK: call @S_op_call_ii({{.*}}) {emitrust.method_call}
// CHECK: call @S_op_eq({{.*}}) {emitrust.method_call}
// CHECK: call @S_op_sub({{.*}}) {emitrust.method_call}
// CHECK: call @M_op_lt({{.*}}) {emitrust.method_call}
// CHECK: call @M_get_i({{.*}}) {emitrust.method_call}
// CHECK: call @M_get_b({{.*}}) {emitrust.method_call}
