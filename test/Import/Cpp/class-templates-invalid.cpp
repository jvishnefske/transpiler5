// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/explicit-spec.cpp 2>&1 | FileCheck %s --check-prefix=EXPLSPEC
// RUN: not emitrust-import-c %t/partial-spec.cpp 2>&1 | FileCheck %s --check-prefix=PARTIAL
// RUN: not emitrust-import-c %t/nttp.cpp 2>&1 | FileCheck %s --check-prefix=NTTP
// RUN: not emitrust-import-c %t/nttp-unused.cpp 2>&1 | FileCheck %s --check-prefix=NTTPUNUSED
// RUN: not emitrust-import-c %t/variadic.cpp 2>&1 | FileCheck %s --check-prefix=VARIADIC
// RUN: not emitrust-import-c %t/member-fn-template.cpp 2>&1 | FileCheck %s --check-prefix=MEMFN
// RUN: not emitrust-import-c %t/static-data-member.cpp 2>&1 | FileCheck %s --check-prefix=STATICMEM
// RUN: not emitrust-import-c %t/collide-template-first.cpp 2>&1 | FileCheck %s --check-prefix=COLTMPL
// RUN: not emitrust-import-c %t/collide-handwritten-first.cpp 2>&1 | FileCheck %s --check-prefix=COLHAND
// RUN: emitrust-cc --recover --emit=rust %t/explicit-spec.cpp | FileCheck %s --check-prefix=EXPLSPECREC --implicit-check-not="v * 2" --implicit-check-not="struct BoxI8"
// RUN: emitrust-cc --recover --emit=rust %t/explicit-spec.cpp 2>&1 >/dev/null | FileCheck %s --check-prefix=EXPLSPECRECDIAG

// W2.16 located-rejection ledger for class templates. W2.16 admits
// exactly ONE shape — a class template whose parameters are all plain
// TYPE parameters, monomorphized by clang into concrete instantiations of
// the PRIMARY template (see class-templates.cpp). Everything else on the
// class-template frontier must stay a LOCATED rejection, and this file is
// the ledger that says so.
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
//    emitting `struct Box_i8` / `struct Fixed_x` for shapes the strict
//    mode rejects. Putting the verdict on the record import itself is
//    what makes all three routes agree.
//
// 2. THE REJECTIONS ARE DRIVEN OFF THE SPECIALIZATION's
//    `TemplateArgument` KIND AND ITS SPECIALIZED-FROM LINK, NEVER OFF
//    ANYTHING IN A BODY. `nttp-unused` below is the regression pin that
//    matters most: `N` appears nowhere in the class, so a body-driven or
//    field-driven check sees an ordinary struct and admits it — under a
//    symbol whose suffix codes the non-type argument as the `x`
//    placeholder, so `Fixed<3>` and `Fixed<40>` SILENTLY MERGE into one
//    struct with one set of methods.
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
//    they emit literally the same code.
//
//    FR-108 RETIRED THE THIRD `collide-*` SECTION. `collide-namespace`
//    pinned `::Box<int>` beside `ns::Box<int>` as a rejection, on the
//    grounds that record names carried NO namespace prefix and so the two
//    patterns composed one spelling. FR-108 gave record names the same
//    `namespacePrefix` `cFunctionSymbolName` has always applied, so those
//    two now COEXIST as `BoxI32` and `NsNsBoxI32` with their own method
//    sets. The rejection was SUPERSEDED, not dropped: its exact input is
//    now a byte-diff EndToEnd leg,
//    test/EndToEnd/cpp-namespace-class-template.cpp. The two sections
//    that remain involve no namespace and must keep failing.
//
// A note on locations: an IMPLICIT instantiation reports the PATTERN's
// `getLocation()`, so all instantiations of one template necessarily
// share one diagnostic location. That is why every check below matches
// the line and column loosely — the file/wording pair is the contract.

//--- explicit-spec.cpp
// An explicit specialization supplies a hand-written class body for one
// argument list, so it is NOT the clang-monomorphized pattern W2.16
// admits: its fields and methods can disagree arbitrarily with the
// primary template's, while both want the same composed struct name.
// REJECT.
// EXPLSPEC: explicit-spec.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: explicit class template specialization
template <typename T>
struct Box {
  T v;
  Box(T x) : v(x) {}
  T get() const { return v; }
};

template <>
struct Box<char> {
  char v;
  Box(char x) : v(x) {}
  char get() const { return (char)(v * 2); }
};

int use(void) {
  Box<char> b('A');
  return (int)b.get();
}

//--- partial-spec.cpp
// A partial specialization is a second PATTERN, selected by argument
// shape. The instantiation `Box<int *>` that hangs off the primary
// template's `specializations()` list is fully concrete and would
// otherwise be admitted as `Box_pi32` — carrying the PARTIAL's body under
// a name computed from the primary. Discriminated on
// `getSpecializedTemplateOrPartial()`, not on dependence (the
// instantiation is not dependent).
// PARTIAL: partial-spec.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: partial class template specialization
template <typename T>
struct Box {
  T v;
  Box(T x) : v(x) {}
  int get() const { return 1; }
};

template <typename T>
struct Box<T *> {
  T *v;
  Box(T *x) : v(x) {}
  int get() const { return 2; }
};

int use(int n) {
  Box<int *> b(&n);
  return b.get();
}

//--- nttp.cpp
// A non-type template parameter is a compile-time VALUE, not a type: the
// suffix scheme codes types only, so two instantiations differing only in
// `N` would fuse onto one struct. REJECT on the argument kind.
// NTTP: nttp.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-type template argument in class template instantiation
template <int N>
struct Fixed {
  int v;
  Fixed(int x) : v(x + N) {}
  int get() const { return v; }
};

int use(int n) {
  Fixed<3> a(n);
  Fixed<40> b(n);
  return a.get() + b.get();
}

//--- nttp-unused.cpp
// REGRESSION PIN. `N` is mentioned NOWHERE in the class, so a field- or
// body-driven rejection sees an ordinary struct and SILENTLY IMPORTS it —
// with a name that does not encode `N`, so `Fixed<3>` and `Fixed<40>` are
// one struct with one constructor. The rejection must come from the
// argument KIND.
// NTTPUNUSED: nttp-unused.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-type template argument in class template instantiation
template <int N>
struct Fixed {
  int v;
  Fixed(int x) : v(x) {}
  int get() const { return v; }
};

int use(int n) {
  Fixed<3> a(n);
  Fixed<40> b(n);
  return a.get() + b.get();
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

// The FR-42 recovery half. The recovered crate must contain a LOUD stub
// and NOTHING of the explicit specialization: the `v * 2` its hand-written
// body spells must not reach emitted Rust under any mode (the two
// `--implicit-check-not`s on the RUN line scan the WHOLE crate for it, not
// just a region), and no `BoxI8` struct may be emitted behind the dropped
// item's back through the on-demand `mapType` route — which is precisely
// what putting the verdict on `importRecordUncached` buys: route (c) hits
// the same rejection, so the recovered crate carries a cascade stub
// instead of a fieldful struct with no methods.
// EXPLSPECREC: unimplemented!("unsupported: struct 'BoxI8' was rejected, so a type naming it cannot be imported")

// All THREE visits reject, and each is LOCATED. The first diagnostic is
// the specialization's own verdict (the `specializations()` walk); the
// second is the ordinary top-level `RecordDecl` visit of the same decl,
// which — because the first visit recorded the rejection — reports the
// cascade wording rather than silently admitting the hand-written body
// (measured pre-guard: it DID admit it, emitting a working
// `Box_i8::box_i8_get` returning `v * 2`).
// EXPLSPECRECDIAG: explicit-spec.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: explicit class template specialization (recovered: item dropped)
// EXPLSPECRECDIAG: explicit-spec.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: struct 'BoxI8' was rejected, so a type naming it cannot be imported
