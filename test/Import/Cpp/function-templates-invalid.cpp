// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/variadic.cpp 2>&1 | FileCheck %s --check-prefix=VARIADIC
// RUN: not emitrust-import-c %t/variadic-no-pack-expr.cpp 2>&1 | FileCheck %s --check-prefix=VARNOPACK
// RUN: not emitrust-import-c %t/nttp-decl.cpp 2>&1 | FileCheck %s --check-prefix=NTTPDECL
// RUN: not emitrust-import-c %t/template-template.cpp 2>&1 | FileCheck %s --check-prefix=TMPLTMPL
// RUN: not emitrust-import-c %t/nttp-recursive.cpp 2>&1 | FileCheck %s --check-prefix=NTTPREC
// RUN: not emitrust-import-c %t/collide-template-first.cpp 2>&1 | FileCheck %s --check-prefix=COLTMPL
// RUN: not emitrust-import-c %t/collide-handwritten-first.cpp 2>&1 | FileCheck %s --check-prefix=COLHAND
// RUN: emitrust-cc --recover --emit=rust %t/variadic-explicit-spec.cpp | FileCheck %s --check-prefix=PACKSPECREC --implicit-check-not="a * b"

// W2.15/W2.28 located-rejection ledger for function templates. W2.15
// admitted plain TYPE arguments; W2.28 widened the frontier with INTEGRAL
// non-type arguments (value-coded suffixes, see function-templates.cpp)
// and EXPLICIT specializations (the hand-written body imports under the
// same suffixed symbol the displaced instantiation would have used).
// Everything still outside — parameter packs, non-INTEGRAL non-type
// arguments (a declaration/pointer NTTP, a template-template argument),
// and self-referential instantiation chains — must stay a LOCATED
// rejection, and this file is the ledger that says so.
//
// Two design decisions are pinned here, both load-bearing:
//
// 1. THE REJECTIONS FIRE ON AN INSTANTIATED SPECIALIZATION'S TEMPLATE
//    ARGUMENT LIST, NOT EAGERLY ON THE UNINSTANTIATED PATTERN. An
//    uninstantiated pattern is never examined at all (that is what keeps
//    test/Import/Cpp/cpp-structured-bindings.cpp's TUPLEPROTO fixture —
//    which declares a never-instantiated `template <size_t I> int get(...)`
//    tuple-protocol overload — importing exactly as it did before W2.15).
//
// 2. THE REJECTIONS ARE DRIVEN OFF THE `TemplateArgument` KIND, NOT OFF
//    ANYTHING IN THE BODY. A body-driven check is measurably
//    insufficient: `variadic-no-pack-expr` below is the shape that
//    SILENTLY IMPORTED under a body-only rejection (a pack with no pack
//    expression in the body), emitting a symbol carrying a placeholder
//    code that two instantiations then collide on. W2.28 retired the
//    NTTP twin of this pin by giving integral values a REAL code
//    (`ident<1>`/`ident<2>` are now `ident_v1`/`ident_v2`, pinned in
//    function-templates.cpp) — the `x` placeholder now stands only for
//    the kinds this file keeps rejecting.
//
// The last RUN line is the FR-42 recovery half of the frontier: a
// hand-written EXPLICIT specialization OF A VARIADIC template is dropped
// with its template, and the recovered crate must carry a loud stub at
// the call — never the specialization's hand-written body admitted
// through the ordinary top-level `FunctionDecl` visit (that visit skips
// every specialization kind; the `--implicit-check-not` scans the whole
// crate for the body's `a * b`).
//
// A note on locations: an IMPLICIT instantiation reports the PATTERN's
// `getLocation()`, so all instantiations of one template necessarily share
// one diagnostic location. That is why every check below matches the line
// and column loosely — the file/wording pair is the contract.

//--- variadic.cpp
// A template parameter pack is a variable-arity argument list: the pack
// expands into a single `TemplateArgument` of kind `Pack`, which the
// suffix scheme has no code for, and the emitted signature would have to
// vary in arity per instantiation. REJECT. (The W2.28 spike measured the
// admission ladder — flattened pack suffixes import the trivial case, but
// the body's `SizeOfPackExpr` is unhandled and recursive expansion
// `sum(rest...)` trips the same-template import-order gap pinned in
// `nttp-recursive.cpp` — so the pack stays rejected as a package.)
// VARIADIC: variadic.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: variadic function template (template parameter pack)
template <typename... Ts>
int count(Ts... ts) {
  return (int)sizeof...(ts);
}

int use(void) {
  return count(1, 2) + count(1, 2, 3);
}

//--- variadic-no-pack-expr.cpp
// REGRESSION PIN. The body contains no pack expression at all (`ts` is
// never expanded, no `sizeof...`), so a body-driven rejection sees an
// ordinary function and SILENTLY IMPORTS it; the two instantiations then
// differ only in the pack and collide.
// VARNOPACK: variadic-no-pack-expr.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: variadic function template (template parameter pack)
template <typename... Ts>
int first(int a, Ts... ts) {
  return a;
}

int use(void) {
  return first(1, 2) + first(1, 2, 3);
}

//--- nttp-decl.cpp
// W2.28 admits INTEGRAL non-type arguments only: they code by VALUE. A
// DECLARATION-kind argument (a pointer/reference NTTP naming a global)
// has no such spelling — two distinct globals would both code `x` and
// fuse — so the kind check keeps rejecting it with the W2.15 wording.
// NTTPDECL: nttp-decl.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-type template argument in function template instantiation
int g = 0;

template <int *P>
int readp(void) {
  return *P;
}

int use(void) {
  return readp<&g>();
}

//--- template-template.cpp
// A template-template argument is a PATTERN, not a type or a value:
// there is nothing monomorphic to code. Same kind-driven rejection.
// TMPLTMPL: template-template.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-type template argument in function template instantiation
template <typename T>
struct Box {
  T v;
};

template <template <typename> class C>
int probe(void) {
  C<int> c;
  c.v = 1;
  return c.v;
}

int use(void) {
  return probe<Box>();
}

//--- nttp-recursive.cpp
// KNOWN GAP, recorded rather than left silent: a SELF-REFERENTIAL
// instantiation chain (`fact<5>` calls `fact<4>`) lists the OUTERMOST
// specialization first in `specializations()`, so its body is imported
// before the specialization it calls exists, and the call site rejects
// with the pre-existing unimported-function wording — loud and located,
// which is all this wave promises for it. (The same walk-order gap is
// why `extern template` declarations surface at their call sites; see
// the W2.15 KNOWN GAPS comment in `importTopLevelDecl`.)
// NTTPREC: nttp-recursive.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to unimported function 'fact_v4'
template <int N>
int fact(int seed) {
  return N <= 1 ? seed : N * fact<N - 1>(seed);
}

template <>
int fact<1>(int seed) {
  return seed;
}

int use(int n) {
  return fact<5>(n);
}

//--- collide-template-first.cpp
// The suffix scheme's type codes are a finite table, so two DISTINCT
// template arguments can still map to one code (two different
// function-pointer types, two different closure types), and an ordinary
// hand-written function may already own the composed spelling. Either way
// the second definition of the symbol must reject with a wording that
// NAMES THE TEMPLATE INSTANTIATION — the generic
// "conflicting definition of 'x' (already defined in another translation
// unit)" is nonsense for a single-TU template clash. Here the template is
// walked first, so the collision surfaces at the hand-written definition.
// COLTMPL: collide-template-first.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: function template instantiation collides with the existing symbol 'add_i32'
template <typename T>
T add(T a, T b) {
  return a + b;
}

int add_i32(int a, int b) {
  return a - b;
}

int use(void) {
  return add(2, 3) + add_i32(2, 3);
}

//--- collide-handwritten-first.cpp
// The mirror order: the hand-written definition is walked first and the
// instantiation collides with it. Same wording, so the diagnostic does
// not depend on declaration order.
// COLHAND: collide-handwritten-first.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: function template instantiation collides with the existing symbol 'add_i32'
int add_i32(int a, int b) {
  return a - b;
}

template <typename T>
T add(T a, T b) {
  return a + b;
}

int use(void) {
  return add(2, 3) + add_i32(2, 3);
}

//--- variadic-explicit-spec.cpp
// The recovery pin: the pack template is dropped, and the explicit
// specialization's hand-written body must NOT be admitted through the
// ordinary top-level visit — the recovered crate carries a loud stub at
// the call instead. (The `count_x` spelling is the pack's `x`
// placeholder code: the symbol exists only inside this rejection
// wording, never as an emitted function.)
// PACKSPECREC: pub fn use_
// PACKSPECREC: unimplemented!("unsupported: call to unimported function 'count_x'")
template <typename... Ts>
int count(Ts... ts) {
  return 0;
}

template <>
int count<int, int>(int a, int b) {
  return a * b;
}

int use(void) {
  return count(1, 2);
}
