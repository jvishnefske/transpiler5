// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | FileCheck %s --check-prefix=NOPATTERN

// W2.15: function-template MONOMORPHIZATION. Clang has already done the
// hard part — by the time `importDeclsIn` walks the translation unit,
// every instantiation a program actually requests exists as a concrete,
// fully-typed `FunctionDecl` hanging off the `FunctionTemplateDecl` with
// a dependent-free body. This file pins that the importer simply IMPORTS
// THOSE, one emitted function per instantiation, and never looks at the
// uninstantiated pattern (which still has dependent types and could not
// be imported at all).
//
// The invariant this file exists to protect is the NAMING one. Two
// instantiations of one template share a C++ spelling (`add` and `add`)
// and even a source location (an implicit instantiation reports the
// PATTERN's `getLocation()`), so without a suffix the second definition
// would collide with the first in the importer's flat `functions` map and
// die with a nonsense cross-TU wording. Hence:
//
// PINNED template-argument suffix scheme (binding; W2.16's class
// templates are required to compose with it — see design.md W2.15/W2.16):
//
//   <the ordinary cFunctionSymbolName base> + ("_" + <code>) per Type
//   template argument, in DECLARATION ORDER of the template parameters.
//
// * The base is exactly what a non-template function of that name would
//   get: `main`-rewrite, Rust-keyword member mangle, namespace flattening
//   prefix, FR-73 underscore fold. The suffix is appended INSIDE
//   `cFunctionSymbolName` (EmitRust/CSymbolNaming.h) and nowhere else, so
//   all ~16 name-RECOMPUTATION sites (call sites, function-pointer
//   resolution, the FR-40 item graph, the planners) derive the suffixed
//   name for free and a call site needs no template-specific handling at
//   all: it already resolves by callee decl identity.
// * The per-argument codes are a table SEPARATE from W2.2's overload
//   codes (`cxxOverloadParamCode`, which spells every integer `i` and is
//   pinned byte-for-byte by test/Import/Cpp/methods.cpp). The template
//   table must be WIDER: `add<int>` and `add<long>` are two different
//   functions and a shared `i` would collide them. The codes are
//   `b`(bool), `i8`/`u8`/`i16`/`u16`/`i32`/`u32`/`i64`/`u64`(integers by
//   width and signedness), `f`(float), `d`(double/long double),
//   `v`(void), `p<pointee-code>`(pointer), the snake_cased tag name for a
//   record/enum, and `x` as the fallback.
// * Every code is snake-safe BY CONSTRUCTION (the record/enum code runs
//   through `toSnakeCase` unconditionally): a `struct P` template
//   argument must emit `sum_p`, never `sum_P`, which rustc's denied
//   non_snake_case lint rejects outright.
//
// Rejections that stay rejections (see function-templates-invalid.cpp):
// explicit specializations, non-type template arguments, and template
// parameter packs. Class templates keep the generic `unsupported
// top-level declaration` (W2.16) — pinned in methods-invalid.cpp.

extern "C" int printf(const char *, ...);

// One type parameter, three integer widths plus two floating widths: the
// whole point of the wider table. `i`/`i` would fuse the first two.
template <typename T>
T add(T a, T b) {
  return a + b;
}

// CHECK-DAG: func.func @add_i32(
// CHECK-DAG: func.func @add_i64(
// CHECK-DAG: func.func @add_f(
// CHECK-DAG: func.func @add_d(

// A bool argument takes the `b` code (distinct from every integer code,
// even though `bool` satisfies `isIntegerType`).
template <typename T>
T pick(T x) {
  return x;
}

// CHECK-DAG: func.func @pick_b(
// CHECK-DAG: func.func @pick_u8(

// Two type parameters: the codes concatenate with one `_` each, in
// template-parameter declaration order, so `cvt<int, double>` and
// `cvt<double, int>` are distinct symbols.
template <typename A, typename B>
int cvt(A a, B b) {
  return (int)a + (int)b;
}

// CHECK-DAG: func.func @cvt_i32_d(
// CHECK-DAG: func.func @cvt_d_i32(

// A record-typed template argument: the code is the snake_cased tag name,
// so the emitted symbol survives rustc's non_snake_case lint.
struct P {
  int x;
};

template <typename T>
int sum(T v) {
  return v.x;
}

// CHECK-DAG: func.func @sum_p(

// A POINTER template argument nests: `p` followed by the pointee's code.
// (Note this is the argument's own shape — `deref<int>` below takes a
// `T *` PARAMETER but its argument is still `int`, so it is `deref_i32`.)
template <typename T>
T deref(T *p) {
  return *p;
}

// CHECK-DAG: func.func @deref_i32(

template <typename T>
int nonnull(T p) {
  return p != 0;
}

// CHECK-DAG: func.func @nonnull_pi32(

// A namespace composes: the flattening prefix goes in front of the base
// name and the suffix behind it, both through the ordinary path.
namespace geo {
template <typename T>
T twice(T v) {
  return v + v;
}
} // namespace geo

// CHECK-DAG: func.func @ns_geo_twice_i32(

int use(int n) {
  int i = add(n, 2);
  long l = add((long)n, 3L);
  float f = add((float)n, 1.5f);
  double d = add((double)n, 2.5);
  bool bb = pick(n > 0);
  unsigned char uc = pick((unsigned char)n);
  int c1 = cvt(n, 1.5);
  int c2 = cvt(1.5, n);
  struct P p;
  p.x = n;
  int s = sum(p);
  int arr[3];
  arr[0] = n;
  int dr = deref(arr);
  int *pn = arr;
  int nn = nonnull(pn);
  int t = geo::twice(n);
  return i + (int)l + (int)f + (int)d + (int)bb + (int)uc + c1 + c2 + s + dr +
         nn + t;
}

// The uninstantiated pattern is never imported: there is no `func.func`
// whose name is the bare template spelling, for any of the templates
// above. (A pattern's parameters are dependent types the importer has no
// mapping for; reaching one at all would be the bug.)
// NOPATTERN-NOT: func.func @add(
// NOPATTERN-NOT: func.func @pick(
// NOPATTERN-NOT: func.func @cvt(
// NOPATTERN-NOT: func.func @sum(
// NOPATTERN-NOT: func.func @deref(
// NOPATTERN-NOT: func.func @nonnull(
// NOPATTERN-NOT: func.func @ns_geo_twice(
