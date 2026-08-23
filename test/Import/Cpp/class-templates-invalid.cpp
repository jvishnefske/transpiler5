// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/nttp-decl.cpp 2>&1 | FileCheck %s --check-prefix=NTTPDECL
// RUN: not emitrust-import-c %t/variadic.cpp 2>&1 | FileCheck %s --check-prefix=VARIADIC
// RUN: not emitrust-import-c %t/member-fn-template.cpp 2>&1 | FileCheck %s --check-prefix=MEMFN
// RUN: not emitrust-import-c %t/static-data-member.cpp 2>&1 | FileCheck %s --check-prefix=STATICMEM
// RUN: not emitrust-import-c %t/collide-template-first.cpp 2>&1 | FileCheck %s --check-prefix=COLTMPL
// RUN: not emitrust-import-c %t/collide-handwritten-first.cpp 2>&1 | FileCheck %s --check-prefix=COLHAND
// RUN: emitrust-cc --recover --emit=rust %t/nttp-decl.cpp | FileCheck %s --check-prefix=NTTPDECLREC --implicit-check-not="struct RX"

// W2.16/W2.28 located-rejection ledger for class templates. W2.16
// admitted plain TYPE arguments monomorphized from the PRIMARY template;
// W2.28 widened the frontier with INTEGRAL non-type arguments
// (value-coded suffixes), EXPLICIT full specializations (the hand-written
// class body imports under the suffixed name its displaced instantiation
// would have used) and instantiations of PARTIAL specializations (fully
// concrete, named by the PRIMARY's argument list) — all pinned
// positively in class-templates.cpp and byte-diffed in
// test/EndToEnd/cpp-template-{nttp,explicit-spec,partial-spec}.cpp.
// Everything still outside — parameter packs, non-INTEGRAL non-type
// arguments — must stay a LOCATED rejection, and this file is the ledger
// that says so.
//
// Three design decisions are pinned here, all load-bearing:
//
// 1. THE REJECTIONS LIVE IN `importRecordUncached`, NOT IN THE
//    `ClassTemplateDecl` ARM. W2.15 learned that an explicit
//    specialization is visited TWICE; for a RECORD it is a TRIPLE visit —
//    (a) the template's `specializations()` walk, (b) the ordinary
//    top-level `RecordDecl` visit (a `ClassTemplateSpecializationDecl`
//    IS-A `RecordDecl`), and (c) ON DEMAND from `mapType` when a local, a
//    member or a parameter names the type. Route (c) is reachable even
//    when FR-42 recovery has dropped (a) and (b), and it was measured
//    emitting `struct Fixed_x` for shapes the strict mode rejects.
//    Putting the verdict on the record import itself is what makes all
//    three routes agree — the recovery RUN line above pins route (c) for
//    the one class-side shape still rejected on argument kind.
//
// 2. THE REJECTIONS ARE DRIVEN OFF THE SPECIALIZATION's
//    `TemplateArgument` KIND, NEVER OFF ANYTHING IN A BODY. The
//    `nttp-decl` section below is the current form of that pin: `P` may
//    appear nowhere in the class body, yet two instantiations at two
//    different globals would both code the `x` placeholder and SILENTLY
//    MERGE into one struct. (W2.28 retired the INTEGRAL twin of this pin
//    by giving values a real code — `Fixed<3>`/`Fixed<40>` are now
//    `Fixed_v3`/`Fixed_v40`, pinned in class-templates.cpp.)
//
// 3. A NAME CLASH INVOLVING AN INSTANTIATION IS A HARD REJECTION, NOT A
//    MERGE. The FR-58 shape dedup keys on the emitted NAME, so a
//    same-shaped hand-written struct that happens to own an
//    instantiation's composed spelling used to be silently unified with
//    it — and because every method mangles as `<StructName>_<method>`,
//    the SECOND type's call sites then resolved to the FIRST type's
//    bodies. That is a silent miscompile (measured: native `1 101`,
//    emitted crate `1 1`), so the `collide-*` sections below pin a
//    located rejection instead. The discriminator is DECL IDENTITY WITHIN
//    ONE TRANSLATION UNIT: the legitimate user of the merge path is the
//    same header template instantiated in several TUs, which must keep
//    merging (pinned by test/EndToEnd and the corpus), and two
//    instantiations of the SAME pattern whose arguments alias onto one
//    type code (`Box<char>`/`Box<signed char>`) must keep merging too —
//    they emit literally the same code. The W2.28 value codes carry a
//    `v` prefix for exactly this family of reasons: a bare-digit code
//    survives snake_case but dies under the UpperCamel record rename,
//    where `Grid<1,23>` and `Grid<12,3>` both camel to `Grid123` and the
//    same-shaped pair was measured dispatching both to one method body
//    (see templateArgIntegralCode and the Duo pin in
//    test/EndToEnd/cpp-template-nttp.cpp).
//
// A note on locations: an IMPLICIT instantiation reports the PATTERN's
// `getLocation()`, so all instantiations of one template necessarily
// share one diagnostic location. That is why every check below matches
// the line and column loosely — the file/wording pair is the contract.

//--- nttp-decl.cpp
// W2.28 admits INTEGRAL non-type arguments only: they code by VALUE. A
// DECLARATION-kind argument (a pointer NTTP naming a global) has no such
// spelling — two distinct globals would both code `x` and fuse — so the
// kind check keeps rejecting it with the W2.16 wording.
// NTTPDECL: nttp-decl.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-type template argument in class template instantiation
int g = 0;

template <int *P>
struct R {
  int get() { return *P; }
};

int use(void) {
  R<&g> r;
  return r.get();
}

//--- variadic.cpp
// A template parameter pack expands into a single `TemplateArgument` of
// kind `Pack`, which the suffix scheme has no code for, and the emitted
// struct's field list would have to vary in arity per instantiation.
// REJECT.
// VARIADIC: variadic.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: variadic class template (template parameter pack)
template <typename... Ts>
struct Tup {
  int n;
  Tup(Ts... ts) : n((int)sizeof...(ts)) {}
  int get() const { return n; }
};

int use(void) {
  Tup<int, double> a(1, 2.5);
  return a.get();
}

//--- member-fn-template.cpp
// KNOWN GAP, not a newly minted wording: a MEMBER function template is
// declared inside the record, so the `ClassTemplateDecl` walk never
// reaches it and `importCXXMethods` has no instantiation surface for it.
// It surfaces at the CALL as the pre-existing unimported-method wording —
// loud and located, which is all this wave promises for it.
// MEMFN: member-fn-template.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to unimported method
template <typename T>
struct Box {
  T v;
  Box(T x) : v(x) {}
  template <typename U>
  U as() const { return (U)v; }
};

int use(int n) {
  Box<int> b(n);
  return (int)b.as<double>();
}

//--- static-data-member.cpp
// KNOWN GAP: a static data member of a class template is an emitted
// GLOBAL, not a field, and the instantiation walk imports records and
// methods only. Since FR-112 the unknown-variable rejection inside
// `get`'s body is CONTAINED -- the method is omitted (warning below,
// carrying the pre-existing wording) and the instantiation itself
// imports -- so the gap now surfaces as the located call-site rejection
// of the omitted method.
// STATICMEM: static-data-member.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: reference to an unknown variable (omitted: method 'get' of class 'Counted_i32')
// STATICMEM: static-data-member.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to unimported method 'Counted_i32_get'
template <typename T>
struct Counted {
  static int count;
  T v;
  Counted(T x) : v(x) {}
  int get() const { return v + count; }
};

template <typename T>
int Counted<T>::count = 0;

int use(int n) {
  Counted<int> c(n);
  return c.get();
}

//--- collide-template-first.cpp
// SILENT-MISCOMPILE PIN (measured at HEAD, before this wave's guard:
// native prints `1 101`, the emitted crate printed `1 1` — the
// hand-written struct's `get` resolved to the instantiation's body). The
// instantiation claims `Box_i32` first; the same-shaped hand-written
// struct that follows must NOT be merged into it.
// COLTMPL: collide-template-first.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: class template instantiation collides with the existing struct 'Box_i32'
extern "C" int printf(const char *, ...);
template <typename T>
struct Box {
  T v;
  Box(T x) : v(x) {}
  int get() const { return (int)v; }
};

struct Box_i32 {
  int v;
  int get() const { return v + 100; }
};

int main(int argc, char **argv) {
  Box<int> b(argc);
  Box_i32 h;
  h.v = argc;
  printf("%d %d\n", b.get(), h.get());
  return 0;
}

//--- collide-handwritten-first.cpp
// The mirror order: the hand-written struct is imported first and the
// instantiation collides with it. Same wording, so the diagnostic does
// not depend on declaration order.
// COLHAND: collide-handwritten-first.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: class template instantiation collides with the existing struct 'Box_i32'
extern "C" int printf(const char *, ...);
struct Box_i32 {
  int v;
  int get() const { return v + 100; }
};

template <typename T>
struct Box {
  T v;
  Box(T x) : v(x) {}
  int get() const { return (int)v; }
};

int main(int argc, char **argv) {
  Box_i32 h;
  h.v = argc;
  Box<int> b(argc);
  printf("%d %d\n", h.get(), b.get());
  return 0;
}

// The FR-42 recovery half of decision 1: the still-rejected NTTP kind is
// dropped and the recovered crate must contain a LOUD cascade stub, never
// a struct emitted behind the dropped item's back through the on-demand
// `mapType` route (the `--implicit-check-not` scans the whole crate for
// the instantiation's would-be spelling).
// NTTPDECLREC: unimplemented!("unsupported: struct 'RX' was rejected, so a type naming it cannot be imported")
