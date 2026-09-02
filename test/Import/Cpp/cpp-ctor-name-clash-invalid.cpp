// FR-185 located-rejection ledger for the CONSTRUCTOR name clash -- the
// hazard that only became reachable when the constructor's fixed base name
// stopped being `new`.
//
// Why the spelling moved: a C++ constructor imports as an INITIALIZER
// method that takes the object as a receiver and returns nothing
// (`fn new(&mut self, i: i32)`). Rust's `new` is a convention with two
// hard rules -- returns `Self`, takes no receiver -- and the emitted
// method breaks both, which default clippy reports as `new_ret_no_self`
// plus `wrong_self_convention` on EVERY emitted C++ class. `ctor` claims
// no convention, so it carries no obligation.
//
// What that costs, and what this file pins: `new` is a C++ keyword, so no
// user member could ever spell it, and the constructor's symbol was
// unclaimable by accident of grammar. `ctor` is an ordinary identifier and
// a member function literally spelled `void ctor()` is legal C++ that
// takes the module symbol `<Struct>_ctor` -- exactly the `dtor`/`drop`
// hazard W2.17 recorded, and exactly why the destructor is not spelled
// `drop` (test/Import/Cpp/destructors-invalid.cpp's DTORCLASH is this
// file's twin). The overload-suffix counter deliberately refuses to fuse a
// constructor with an identifier-named sibling -- they are different
// `DeclarationName` shapes and are never one overload set -- so the two
// compose ONE symbol, and at EQUAL signatures the FR-47 prepass
// reconciliation would merge them SILENTLY and every `s.ctor()` call would
// run the constructor body. Nothing may silently emit wrong code: the
// pairing is rejected LOCATED at the constructor, class-level like the
// destructor's clash (a constructor is invoked implicitly, so there is no
// call node an FR-112 omission could hang a use-site rejection on).
//
// The three NON-rejections below are as load-bearing as the rejection: the
// guard compares the POST-FOLD in-impl spelling, so it must not fire on a
// class whose constructors genuinely suffix away from a bare `ctor`
// sibling, nor on a `ctor` member in a class with no constructor at all.
// The fold-sensitivity is by construction (the same shape FR-125's
// qualified-owner guard has): `void Ctor()` collides only in the mode
// where the idiomatic rename exists to fold it.

// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/clash.cpp 2>&1 | FileCheck %s --check-prefix=CLASH
// RUN: not emitrust-cc --emit=rust %t/fold-clash.cpp 2>&1 | FileCheck %s --check-prefix=FOLD
// RUN: emitrust-import-c %t/fold-clash.cpp 2>&1 | FileCheck %s --check-prefix=NOFOLD
// RUN: emitrust-import-c %t/ctor-only.cpp 2>&1 | FileCheck %s --check-prefix=CTORONLY
// RUN: emitrust-import-c %t/overload-split.cpp 2>&1 | FileCheck %s --check-prefix=SPLIT

//--- clash.cpp
// The signature-EQUAL pairing, which is the silent-merge shape: `S()` and
// `void ctor()` both map to `(&mut S) -> ()` under the one symbol
// `S_ctor`. Rejected at the constructor.
// CLASH: clash.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: constructor collides with the member function 'ctor'
extern "C" int printf(const char *, ...);
struct S {
  int v;
  S() : v(1) {}
  void ctor() { printf("manual %d\n", v); }
};
int use(void) {
  S s;
  s.ctor();
  return s.v;
}

//--- fold-clash.cpp
// The rename-only half: `Ctor` and the constructor's `ctor` are distinct
// spellings that the idiomatic rename folds onto one symbol, so the clash
// exists in `emitrust-cc`'s mode and NOT in the importer's verbatim one.
// The guard is keyed on the folded spelling, so it is mode-sensitive by
// construction -- the same posture FR-125's qualified-owner guard has.
// FOLD: fold-clash.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: constructor collides with the member function 'Ctor'
// NOFOLD-NOT: error
// NOFOLD-DAG: func.func @S_ctor(%{{.*}}: !emitrust.mut_ref<!emitrust.struct<"S">>, %{{.*}}: i32)
// NOFOLD-DAG: func.func @S_Ctor(%{{.*}}: !emitrust.mut_ref<!emitrust.struct<"S">>)
extern "C" int printf(const char *, ...);
struct S {
  int v;
  S(int i) : v(i) {}
  void Ctor() { printf("manual %d\n", v); }
};
int use(void) {
  S s(1);
  s.Ctor();
  return s.v;
}

//--- ctor-only.cpp
// No constructor at all: `ctor` is an ordinary member and owns the symbol.
// Nothing to collide with, so nothing is rejected.
// CTORONLY-NOT: error
// CTORONLY: func.func @S_ctor(%{{.*}}: !emitrust.mut_ref<!emitrust.struct<"S">>) attributes {emitrust.method_of = "S", emitrust.method_rust_name = "ctor"}
extern "C" int printf(const char *, ...);
struct S {
  int v;
  void ctor() { printf("manual %d\n", v); }
};
int use(void) {
  S s;
  s.v = 1;
  s.ctor();
  return s.v;
}

//--- overload-split.cpp
// Two constructors form a genuine overload set, so BOTH take FR-114
// suffixes (`S_ctor_i`, `S_ctor_d`) and neither lands on the bare
// `S_ctor` the member owns. The guard compares suffixed spellings, so it
// must NOT fire here -- a literal `"ctor"` test (the destructor guard's
// shape) would over-reject this class.
// SPLIT-NOT: error
// SPLIT-DAG: func.func @S_ctor_i(%{{.*}}: !emitrust.mut_ref<!emitrust.struct<"S">>, %{{.*}}: i32)
// SPLIT-DAG: func.func @S_ctor_d(%{{.*}}: !emitrust.mut_ref<!emitrust.struct<"S">>, %{{.*}}: f64)
// SPLIT-DAG: func.func @S_ctor(%{{.*}}: !emitrust.mut_ref<!emitrust.struct<"S">>) attributes {emitrust.method_of = "S", emitrust.method_rust_name = "ctor"}
extern "C" int printf(const char *, ...);
struct S {
  int v;
  S(int i) : v(i) {}
  S(double d) : v((int)d) {}
  void ctor() { printf("manual %d\n", v); }
};
int use(void) {
  S s(1);
  s.ctor();
  return s.v;
}
