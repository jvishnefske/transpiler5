// RUN: emitrust-import-c %s | FileCheck %s

// FR-114: overloaded FREE functions and same-arity overloaded CONSTRUCTORS
// get distinguishing symbols instead of colliding on one emitted name.
// This file pins the POSITIVE half of the fix; the still-colliding shapes
// stay located rejections in overload-collisions-invalid.cpp.
//
// What is pinned, and why each pin is load-bearing:
//
// 1. FREE-FUNCTION SUFFIX: `g(int)`/`g(double)` emit `g_i32`/`g_d` — one
//    W2.15-style `_<code>` per parameter (templateArgTypeCode's table),
//    applied ONLY when the declaration context holds a genuine overload
//    set (>1 same-named non-template FunctionDecl). A sole function keeps
//    its historical bare name — `solo` below has a parameter but NO
//    suffix, which is the zero-golden-churn guarantee for every existing
//    C and C++ test. The suffix is computed inside `cFunctionSymbolName`
//    (EmitRust/CSymbolNaming.h), NOT in the importer's definition path,
//    for W2.15's load-bearing reason: the FR-40 item graph and every call
//    site recompute the symbol through that one function, so all of them
//    agree on the suffixed spelling BY CONSTRUCTION. Before FR-114 this
//    pair rejected with a cross-TU wording that blamed the wrong thing
//    and, under --incremental, stubbed c_main itself.
//
// 2. ZERO-PARAM OVERLOADS KEEP THE BARE NAME (raytracing's worked
//    example): `random_double()` stays `random_double` (empty suffix by
//    loop construction — no parameters, no `_<code>` appended), while
//    `random_double(double, double)` takes `random_double_d_d`. A
//    zero-parameter C++ signature is unique within its overload set by
//    construction, so the bare name cannot collide.
//
// 3. WIDENED MEMBER x-FALLBACK: the frozen W2.2 member codes `b`/`i` are
//    untouched (test/Import/Cpp/methods.cpp pins Counter_ctor_i and
//    Counter_get_i byte-for-byte), but everything that previously fell to
//    the `x` placeholder now delegates to templateArgTypeCode: `double`
//    codes `d`, and a REFERENCE parameter takes a new `r` prefix over its
//    referenced type's code (`const S &` -> `rs`). The motivating pair —
//    interval's `S(double,double)` vs `S(const S&,const S&)`, both `xx`
//    before FR-114 — disambiguates as S_ctor_dd / S_ctor_rsrs.
//
// 4. ENUM CARVE-OUT: an enum satisfies `isIntegerType`, so without its
//    own arm it would code `i` and collide with a genuine int overload.
//    It takes its tag-name code instead (the same choice W2.15 made in
//    templateArgTypeCode): `h(int)`/`h(Color)` -> M_h_i / M_h_color.
//    Post-FR-113 the enum-argument call imports cleanly, so this is a
//    positive pin, not a frontier one.
//
// The `S(const S&, const S&)` ctor is DEFINED but never CALLED here: a
// struct lvalue passed to a const-T& constructor parameter is a
// pre-existing borrow-insertion gap proved independent of naming by the
// FR-114 spike (small mirror fix, separate increment). The symbol and
// signature are what FR-114 owns, and they are pinned below.

extern "C" int printf(const char *, ...);

// -- 1: the free pair -----------------------------------------------------
int g(int x) { return x * 2; }
double g(double x) { return x * 2.0; }

// CHECK-LABEL: func.func @g_i32(
// CHECK-SAME: i32
// CHECK-LABEL: func.func @g_d(
// CHECK-SAME: f64

// A sole (non-overloaded) function keeps the bare name despite having
// parameters: no overload set, no suffix.
int solo(int x) { return x + 1; }
// CHECK-LABEL: func.func @solo(

// -- 2: zero-param overload keeps the bare name ---------------------------
double random_double() { return 0.5; }
double random_double(double lo, double hi) { return lo + hi; }
// CHECK-LABEL: func.func @random_double()
// CHECK-LABEL: func.func @random_double_d_d(

// -- 4: enum carve-out on the member path ---------------------------------
enum Color { Red, Green };

class M {
public:
  int t;
  int h(int x) { return t + x; }
  int h(Color c) {
    if (c == Red)
      return t;
    return 0;
  }
};
// CHECK-LABEL: func.func @M_h_i(
// CHECK-SAME: !emitrust.mut_ref<!emitrust.struct<"M">>
// CHECK-SAME: i32
// CHECK-LABEL: func.func @M_h_color(
// CHECK-SAME: !emitrust.enum<"Color">

// -- 3: the motivating constructor pair -----------------------------------
class S {
public:
  double a;
  double b;
  S(double x, double y) : a(x), b(y) {}
  S(const S &p, const S &q) : a(p.a), b(q.b) {}
};
// CHECK-LABEL: func.func @S_ctor_dd(
// CHECK-SAME: !emitrust.mut_ref<!emitrust.struct<"S">>
// CHECK-SAME: f64, {{.*}}f64
// CHECK-LABEL: func.func @S_ctor_rsrs(
// CHECK-SAME: !emitrust.mut_ref<!emitrust.struct<"S">>
// CHECK-SAME: !emitrust.ref<!emitrust.struct<"S">>
// CHECK-SAME: !emitrust.ref<!emitrust.struct<"S">>

// -- call sites resolve to the suffixed symbols ---------------------------
int use_all(int seed) {
  int a = g(seed);
  double d = g(seed * 2.0);
  double r0 = random_double();
  double r2 = random_double(r0, d);
  M m;
  m.t = seed;
  int hi = m.h(seed);
  int hc = m.h(Red);
  S s(r2, d);
  return a + hi + hc + (int)s.a;
}
// CHECK-LABEL: func.func @use_all
// CHECK: call @g_i32(
// CHECK: call @g_d(
// CHECK: call @random_double()
// CHECK: call @random_double_d_d(
// CHECK: call @M_h_i(
// CHECK: call @M_h_color(
// CHECK: call @S_ctor_dd(
