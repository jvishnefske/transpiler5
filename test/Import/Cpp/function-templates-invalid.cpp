// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/explicit-spec.cpp 2>&1 | FileCheck %s --check-prefix=EXPLSPEC
// RUN: not emitrust-import-c %t/nttp.cpp 2>&1 | FileCheck %s --check-prefix=NTTP
// RUN: not emitrust-import-c %t/nttp-unused.cpp 2>&1 | FileCheck %s --check-prefix=NTTPUNUSED
// RUN: not emitrust-import-c %t/variadic.cpp 2>&1 | FileCheck %s --check-prefix=VARIADIC
// RUN: not emitrust-import-c %t/variadic-no-pack-expr.cpp 2>&1 | FileCheck %s --check-prefix=VARNOPACK
// RUN: not emitrust-import-c %t/collide-template-first.cpp 2>&1 | FileCheck %s --check-prefix=COLTMPL
// RUN: not emitrust-import-c %t/collide-handwritten-first.cpp 2>&1 | FileCheck %s --check-prefix=COLHAND
// RUN: emitrust-cc --recover --emit=rust %t/explicit-spec.cpp | FileCheck %s --check-prefix=EXPLSPECREC

// W2.15 located-rejection ledger for function templates. W2.15 admits
// exactly ONE shape — a template whose parameters are all plain TYPE
// parameters, monomorphized by clang into concrete instantiations (see
// function-templates.cpp). Everything else on the template frontier must
// stay a LOCATED rejection, and this file is the ledger that says so.
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
//    ANYTHING IN THE BODY. A body-driven check is measurably insufficient:
//    the `nttp-unused` and `variadic-no-pack-expr` sections below are the
//    two shapes that SILENTLY IMPORTED under a body-only rejection (a pack
//    with no `sizeof...` in the body, and an NTTP whose `N` is never
//    mentioned), each emitting a symbol carrying a placeholder code that
//    two instantiations would then collide on. They are the regression
//    pins that matter most in this file.
//
// The last RUN line is the FR-42 recovery half of decision 1's corollary.
// An explicit specialization is reached TWICE; under `--recover` the
// template item is dropped, and without a guard on the ordinary
// top-level `FunctionDecl` visit the specialization's hand-written body
// would then be admitted SILENTLY (measured: the recovered crate
// contained a working `fn add_i32 { a * b }`). Recovery must fail loudly
// instead, per CLAUDE.md's "a recovered item that reaches emission
// unresolved must fail loudly there".
//
// A note on locations: an IMPLICIT instantiation reports the PATTERN's
// `getLocation()`, so all instantiations of one template necessarily share
// one diagnostic location. That is why every check below matches the line
// and column loosely — the file/wording pair is the contract.

//--- explicit-spec.cpp
// An explicit specialization supplies a hand-written body for one
// argument list, so it is NOT the clang-monomorphized pattern W2.15
// admits: emitting it would need the specialization's own body imported
// under the generic instantiation's symbol, and the two can disagree
// arbitrarily. REJECT. The specialization decl is visited TWICE (once
// through the template's `specializations()` list, once as an ordinary
// top-level `FunctionDecl`), and BOTH visits reject — a guard that
// matters under `--recover`, where dropping the template item would
// otherwise let the ordinary visit silently admit the specialization.
// EXPLSPEC: explicit-spec.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: explicit function template specialization
template <typename T>
T add(T a, T b) {
  return a + b;
}

template <>
int add<int>(int a, int b) {
  return a * b;
}

int use(void) {
  return add<int>(2, 3);
}

//--- nttp.cpp
// A non-type template parameter is a compile-time VALUE, not a type: the
// suffix scheme codes types only, so two instantiations differing only in
// `N` would fuse. REJECT on the argument kind.
// NTTP: nttp.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-type template argument in function template instantiation
template <int N>
int scale(int x) {
  return x * N;
}

int use(void) {
  return scale<3>(4) + scale<5>(4);
}

//--- nttp-unused.cpp
// REGRESSION PIN. `N` is never mentioned in the body, so a body-driven
// rejection sees nothing wrong and this SILENTLY IMPORTS — with a symbol
// that does not encode `N`, so `ident<1>` and `ident<2>` are one function.
// The rejection must therefore come from the argument KIND.
// NTTPUNUSED: nttp-unused.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-type template argument in function template instantiation
template <int N>
int ident(int x) {
  return x;
}

int use(void) {
  return ident<1>(4) + ident<2>(4);
}

//--- variadic.cpp
// A template parameter pack is a variable-arity argument list: the pack
// expands into a single `TemplateArgument` of kind `Pack`, which the
// suffix scheme has no code for, and the emitted signature would have to
// vary in arity per instantiation. REJECT.
// VARIADIC: variadic.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: variadic function template (template parameter pack)
template <typename... Ts>
int count(Ts... ts) {
  return (int)sizeof...(ts);
}

int use(void) {
  return count(1, 2) + count(1, 2, 3);
}

//--- variadic-no-pack-expr.cpp
// REGRESSION PIN, the pack twin of `nttp-unused`. The body contains no
// pack expression at all (`ts` is never expanded, no `sizeof...`), so a
// body-driven rejection sees an ordinary function and SILENTLY IMPORTS
// it; the two instantiations then differ only in the pack and collide.
// VARNOPACK: variadic-no-pack-expr.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: variadic function template (template parameter pack)
template <typename... Ts>
int first(int a, Ts... ts) {
  return a;
}

int use(void) {
  return first(1, 2) + first(1, 2, 3);
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

// The recovered crate must contain a LOUD stub for the specialization,
// never its hand-written body: the `a * b` the explicit specialization
// spells must not reach emitted Rust under any mode.
// EXPLSPECREC: pub fn add_i32
// EXPLSPECREC-NEXT: unimplemented!("unsupported: explicit function template specialization")
// EXPLSPECREC-NOT: a * b
