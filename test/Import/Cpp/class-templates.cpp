// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | FileCheck %s --check-prefix=NOPATTERN

// W2.16: class-template MONOMORPHIZATION. Exactly as in W2.15's function
// case, clang has already done the hard part — by the time
// `importDeclsIn` walks the translation unit, every class-template
// instantiation the program requests exists as a concrete
// `ClassTemplateSpecializationDecl` (which IS-A `CXXRecordDecl`) with
// fully substituted fields and instantiated `CXXConstructorDecl` /
// `CXXMethodDecl` members hanging off the `ClassTemplateDecl`. This file
// pins that the importer simply IMPORTS THOSE, one emitted struct per
// instantiation, through W2.2's existing `importRecord` +
// `importCXXMethods` surface UNCHANGED, and never looks at the
// uninstantiated pattern (whose fields are dependent types with no
// mapping).
//
// The invariant this file exists to protect is the NAMING one, and it is
// the mirror image of function-templates.cpp's:
//
// PINNED class-template struct-name scheme (binding; composes with
// W2.15's function suffix — see design.md W2.15/W2.16):
//
//   <the ordinary recordRustName base> + ("_" + <code>) per Type
//   template argument, in DECLARATION ORDER of the template parameters,
//   the whole spelling THEN passed through the idiomatic type rename.
//
// * The per-argument codes are W2.15's table verbatim
//   (`templateArgTypeCode` in EmitRust/CSymbolNaming.h): `b`, `i8`/`u8`/
//   `i16`/`u16`/`i32`/`u32`/`i64`/`u64`, `f`, `d`, `v`,
//   `p<pointee-code>`, the snake_cased tag name for a record/enum, `x`
//   as the fallback. Reusing it (rather than minting a second table) is
//   what keeps `Box<int>` and `add<int>` coding the same argument the
//   same way.
// * The suffix is appended INSIDE `recordRustName` and nowhere else, so
//   all 8 name-RECOMPUTATION sites (`structSymbolName`, the rejected-
//   record wording, `importRecordUncached`, `emittedRecordName`, the two
//   FR-42 recovery sites, the FR-40 item graph, the coloring probe)
//   derive the suffixed name for free. That is what makes a LOCAL, a
//   MEMBER, a by-value PARAMETER, a reference parameter, a RETURN type
//   and an ARRAY element of `Box<int>` all resolve without one line of
//   template-specific code, and it is what feeds W2.2's per-class method
//   mangle (`cxxMethodMangledName` reads the assigned struct name out of
//   the `assignedStructNames` cache, so `Box_i32_get` falls out with no
//   edit at all).
// * ORDERING IS LOAD-BEARING AND INVERTED VERSUS W2.15. The function
//   suffix goes on AFTER `mangleMemberName`'s snake-case spelling; the
//   record suffix must go on BEFORE `toUpperCamelCase`. Appended after
//   the rename, `Box<int>` would emit as `Box_i32`, which rustc's denied
//   `non_camel_case_types` lint rejects outright. Pre-rename it becomes
//   `BoxI32` — lint-clean, and still injective per code. (This file runs
//   `emitrust-import-c`, where the idiomatic rename is OFF, so the names
//   below are the pre-rename spellings verbatim; the post-rename
//   spelling is pinned by test/EndToEnd/cpp-class-template.cpp and
//   test/Project/item-graph-class-template.cpp.)
//
// Rejections that stay rejections (see class-templates-invalid.cpp):
// explicit and partial specializations, non-type template arguments,
// parameter packs, and member function templates.

#include <utility>
#include <vector>

extern "C" int printf(const char *, ...);

// One type parameter, a constructor with a member-initializer list, a
// const method and a mutating method: the whole W2.2 method surface,
// reached through an instantiation instead of a hand-written class.
template <typename T>
struct Box {
  T v;
  Box(T x) : v(x) {}
  T get() const { return v; }
  void bump(T d) { v = v + d; }
};

// CHECK-DAG: emitrust.struct_def @Box_i32 ["v"] [i32]
// CHECK-DAG: emitrust.struct_def @Box_d ["v"] [f64]
// CHECK-DAG: func.func @Box_i32_new(
// CHECK-DAG: func.func @Box_i32_get(
// CHECK-DAG: func.func @Box_i32_bump(
// CHECK-DAG: func.func @Box_d_new(
// CHECK-DAG: func.func @Box_d_get(
// CHECK-DAG: func.func @Box_d_bump(

// Two type parameters: the codes concatenate with one `_` each, in
// template-parameter declaration order, so `Pair2<int, double>` and
// `Pair2<double, int>` are distinct structs with distinct field types —
// the shape that dies if the codes are joined in any other order.
template <typename A, typename B>
struct Pair2 {
  A a;
  B b;
  Pair2(A x, B y) : a(x), b(y) {}
  A first() const { return a; }
  B second() const { return b; }
};

// CHECK-DAG: emitrust.struct_def @Pair2_i32_d ["a", "b"] [i32, f64]
// CHECK-DAG: emitrust.struct_def @Pair2_d_i32 ["a", "b"] [f64, i32]
// CHECK-DAG: func.func @Pair2_i32_d_first(
// CHECK-DAG: func.func @Pair2_d_i32_first(

// A plain struct with a member OF an instantiation: the member's type is
// resolved through `emittedRecordName`, one of the 8 recomputation sites
// the in-`recordRustName` placement covers. It is never constructed here
// on purpose — file-scope records import EAGERLY, so the member-type
// resolution is pinned without dragging in a member-initializer
// constructor (a pre-existing gap, unrelated to templates).
struct Holder {
  Box<int> inner;
};

// CHECK-DAG: emitrust.struct_def @Holder ["inner"] [!emitrust.struct<"Box_i32">]

// A by-value parameter, a reference parameter and a return type, all
// naming instantiations.
int sum_boxes(Box<int> x, Box<int> &y) { return x.get() + y.get(); }

Box<double> make_box(double d) {
  Box<double> b(d);
  return b;
}

int use(int n) {
  Box<int> bi(n);
  Box<double> bd(1.25);
  bi.bump(3);
  bd.bump(0.25);
  Box<int> other(n + 1);
  Pair2<int, double> p(n, 2.5);
  Pair2<double, int> q(0.5, n);
  Box<double> md = make_box(2.5);
  return bi.get() + (int)bd.get() + sum_boxes(bi, other) + p.first() +
         (int)q.first() + (int)md.get();
}

// STL NON-REGRESSION, in the SAME translation unit as the user templates
// above. `std::vector<int>` and `std::pair<int, double>` are ALSO
// `ClassTemplateSpecializationDecl`s, so the new arm must not intercept
// them: `mapStdLibraryType` stays the authoritative route for anything in
// namespace `std`. Two independent guards keep it that way — the
// `ClassTemplateDecl`s themselves live in system headers, so
// `importDeclsIn`'s `isSystemHeaderDecl` skip means the walk never sees
// them, and `mapType` diverts on `isInStdNamespace()` before the generic
// record path. The pin below is the sharp one: `std::pair<int, double>`
// keeps the name `mapStdLibraryType` PRE-SEEDS into `assignedStructNames`
// (`Pair_<rust-element-spelling>`, i.e. `Pair_i32_f64`), NOT the
// `pair_i32_d` that this wave's `recordRustName` suffix would compute for
// it. The two spellings differ exactly on `f64` vs `d`, so this check
// fails the moment the suffix starts governing an STL type.
int use_stl(int n) {
  std::vector<int> vi;
  vi.push_back(n);
  std::pair<int, double> pr(n, 1.5);
  return vi[0] + pr.first + (int)pr.second;
}

// CHECK-DAG: emitrust.struct_def @Pair_i32_f64 ["first", "second"] [i32, f64]
// CHECK-DAG: emitrust.variable named "vi" : !emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>

// NOPATTERN-NOT: emitrust.struct_def @Pair_i32_d {{\[}}
// NOPATTERN-NOT: emitrust.struct_def @vector{{.*}}
// NOPATTERN-NOT: emitrust.struct_def @pair{{.*}}

// The uninstantiated pattern is never imported: there is no struct_def
// and no method under the bare template spelling, for either template
// above. (A pattern's fields are dependent types the importer has no
// mapping for; reaching one at all would be the bug.)
// NOPATTERN-NOT: emitrust.struct_def @Box {{\[}}
// NOPATTERN-NOT: emitrust.struct_def @Pair2 {{\[}}
// NOPATTERN-NOT: func.func @Box_new(
// NOPATTERN-NOT: func.func @Box_get(
// NOPATTERN-NOT: func.func @Pair2_first(

// ---- W2.28: the class-side frontier widening ----
//
// An INTEGRAL non-type argument codes by VALUE through the same
// `templateArgIntegralCode` the function suffix uses (`v3`, `vn3` for a
// negative; the `v` prefix is what survives the UpperCamel rename —
// see class-templates-invalid.cpp decision 3). `Fixed<3>`/`Fixed<40>`
// was W2.16's nttp-unused rejection input; the value code is what
// retired that pin: two instantiations, two structs, two method sets.
template <int N>
struct FixedW {
  int v;
  int cap() { return N; }
};

// CHECK-DAG: emitrust.struct_def @FixedW_v3 ["v"] [i32]
// CHECK-DAG: emitrust.struct_def @FixedW_v40 ["v"] [i32]
// CHECK-DAG: func.func @FixedW_v3_cap(
// CHECK-DAG: func.func @FixedW_v40_cap(

// An EXPLICIT full specialization imports its HAND-WRITTEN class body —
// including a field set that disagrees with the primary's — under the
// suffixed name its displaced instantiation would have used.
template <typename T>
struct TagW {
  T v;
  int id() { return 1; }
};

template <>
struct TagW<int> {
  int v;
  int bonus;
  int id() { return v + bonus; }
};

// CHECK-DAG: emitrust.struct_def @TagW_i32 ["v", "bonus"] [i32, i32]
// CHECK-DAG: emitrust.struct_def @TagW_i8 ["v"] [i8]

// An instantiation whose pattern is a PARTIAL specialization is fully
// concrete and imports carrying the PARTIAL's body (its own field set,
// its own methods) under the PRIMARY template's argument suffix — the
// partial PATTERN itself is dependent and is skipped structurally, never
// imported, never rejected.
template <typename T>
struct PW {
  int t;
  int k() { return 1; }
};

template <typename T>
struct PW<T *> {
  T *p;
  int k() { return 2; }
};

// CHECK-DAG: emitrust.struct_def @PW_i32 ["t"] [i32]
// CHECK-DAG: emitrust.struct_def @PW_pi32 ["p"]
// CHECK-DAG: func.func @PW_i32_k(
// CHECK-DAG: func.func @PW_pi32_k(

int use2(int n) {
  FixedW<3> f3;
  FixedW<40> f40;
  f3.v = n;
  f40.v = n;
  TagW<int> ti;
  ti.v = n;
  ti.bonus = 40;
  TagW<char> tc;
  tc.v = (char)n;
  PW<int> w1;
  w1.t = n;
  PW<int *> w2;
  w2.p = &n;
  return f3.cap() + f40.cap() + ti.id() + tc.id() + w1.k() + w2.k();
}

// NOPATTERN-NOT: emitrust.struct_def @FixedW {{\[}}
// NOPATTERN-NOT: emitrust.struct_def @TagW {{\[}}
// NOPATTERN-NOT: emitrust.struct_def @PW {{\[}}
