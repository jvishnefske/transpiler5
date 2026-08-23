// FR-108 located-rejection ledger for record NAME collisions (the
// non-template half of W2.16's channel). Two DIFFERENT file-scope records
// in ONE translation unit that compute the same EMITTED name used to be
// merged silently by the shape-keyed dedup in `importRecordUncached`: the
// second definition returned success before its methods were ever
// imported, and because every method mangles `<StructName>_<method>`,
// the second type's call sites resolved to the FIRST type's bodies.
// Measured at HEAD: `struct box_i32` beside `struct BoxI32` prints
// `101 1` natively and `101 101` from the emitted crate. No templates
// involved; nothing was diagnosed.
//
// Two design decisions are pinned here:
//
// 1. REJECT, DO NOT RENAME. An injective naming scheme is not available:
//    `ItemGraphBuilder::recordSymbolFor` and FR-41's coloring probe
//    recompute record symbols from the AST ALONE, per TU, with no
//    importer state, so any order-dependent disambiguator ("the second
//    one gets a 2") is unreproducible in the item graph and across the
//    cross-TU merge — it would break the CSymbolNaming.h byte-identity
//    invariant. The only AST-pure injective spelling is the raw C one,
//    which is exactly what `--preserve-c-names` already emits (pinned by
//    the PRESERVED leg below).
//
// 2. THE GUARD IS KEYED ON THE EMITTED NAME, so it is naturally
//    mode-sensitive: `box_i32`/`BoxI32` only collide under the FR-53
//    idiomatic rename, and the rename-OFF mode keeps transpiling them.
//    A blanket "reject any name reuse" would have regressed that.
//
// The namespace flavor of the same channel is NOT here: FR-108 gives
// record names `namespacePrefix`, so `::Box` and `ns::Box` COEXIST (see
// test/EndToEnd/cpp-namespace-records.cpp). What the prefix does create
// is a fresh collision opportunity between a namespaced record and a
// hand-written global spelled like its composed name — the PREFIX legs.
// RUN: split-file %s %t
// RUN: not emitrust-cc --emit=rust %t/rename-collide.cpp 2>&1 | FileCheck %s --check-prefix=RENAME
// RUN: not emitrust-cc --emit=rust %t/rename-collide-reverse.cpp 2>&1 | FileCheck %s --check-prefix=RENAMEREV
// RUN: emitrust-import-c %t/rename-collide.cpp | FileCheck %s --check-prefix=PRESERVED
// RUN: not emitrust-cc --emit=rust %t/rename-diffshape.cpp 2>&1 | FileCheck %s --check-prefix=DIFFSHAPE
// RUN: not emitrust-import-c %t/prefix-collide.cpp 2>&1 | FileCheck %s --check-prefix=PREFIX
// RUN: not emitrust-cc --emit=rust %t/prefix-collide.cpp 2>&1 | FileCheck %s --check-prefix=PREFIXRENAME
// RUN: not emitrust-import-c %t/prefix-collide-reverse.cpp 2>&1 | FileCheck %s --check-prefix=PREFIXREV
// RUN: emitrust-cc --recover --emit=rust %t/rename-collide.cpp | FileCheck %s --check-prefix=RENAMEREC --implicit-check-not="7i32"
// RUN: emitrust-cc --recover --emit=rust %t/rename-collide.cpp 2>&1 >/dev/null | FileCheck %s --check-prefix=RENAMERECDIAG

//--- rename-collide.cpp
// SILENT-MISCOMPILE PIN (measured at HEAD: native `101 1`, emitted crate
// `101 101` — `BoxI32::get`'s body was never emitted at all). The tag
// `box_i32` claims the emitted name `BoxI32` first; the hand-written
// `BoxI32` that follows must NOT be merged into it.
// RENAME: rename-collide.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct 'BoxI32' collides with the emitted name of a different struct in this translation unit
extern "C" int printf(const char *, ...);
struct box_i32 {
  int v;
  int get() const { return v + 100; }
};

struct BoxI32 {
  int v;
  int get() const { return v + 7; }
};

int main(int argc, char **argv) {
  box_i32 a;
  a.v = argc;
  BoxI32 b;
  b.v = argc;
  printf("%d %d\n", a.get(), b.get());
  return 0;
}

//--- rename-collide-reverse.cpp
// The mirror order: `BoxI32` is imported first and `box_i32` collides
// with it. Same wording, so the diagnostic does not depend on
// declaration order.
// RENAMEREV: rename-collide-reverse.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct 'BoxI32' collides with the emitted name of a different struct in this translation unit
extern "C" int printf(const char *, ...);
struct BoxI32 {
  int v;
  int get() const { return v + 7; }
};

struct box_i32 {
  int v;
  int get() const { return v + 100; }
};

int main(int argc, char **argv) {
  BoxI32 b;
  b.v = argc;
  box_i32 a;
  a.v = argc;
  printf("%d %d\n", a.get(), b.get());
  return 0;
}

//--- rename-diffshape.cpp
// The SAME two tags with DIFFERENT field shapes. This shape already
// failed loudly, but with a factually wrong wording — "in another
// translation unit" on a single-file input, because the only clash the
// shape-keyed dedup could imagine was the cross-TU header merge. The
// same-TU guard runs first now, so the diagnostic tells the truth.
// DIFFSHAPE: rename-diffshape.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct 'BoxI32' collides with the emitted name of a different struct in this translation unit
// DIFFSHAPE-NOT: another translation unit
extern "C" int printf(const char *, ...);
struct box_i32 {
  int v;
  int w;
  int get() const { return v + 100; }
};

struct BoxI32 {
  int v;
  int get() const { return v + 7; }
};

int main(int argc, char **argv) {
  box_i32 a;
  a.v = argc;
  a.w = 0;
  BoxI32 b;
  b.v = argc;
  printf("%d %d\n", a.get(), b.get());
  return 0;
}

//--- prefix-collide.cpp
// FR-108's namespace prefix CREATES this shape: `ns::box` composes the
// emitted name `ns_ns_box`, which a hand-written global tag may already
// spell literally. The two are different C++ types and must not share a
// Rust type — so the same-TU guard is mandatory even though the prefix
// is what makes `::Box`/`ns::Box` coexist.
// PREFIX: prefix-collide.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct 'ns_ns_box' collides with the emitted name of a different struct in this translation unit
// Under the idiomatic rename both compose `NsNsBox` instead; the guard
// quotes whichever spelling the mode emits.
// PREFIXRENAME: prefix-collide.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct 'NsNsBox' collides with the emitted name of a different struct in this translation unit
extern "C" int printf(const char *, ...);
struct ns_ns_box {
  int v;
  int get() const { return v; }
};

namespace ns {
struct box {
  int v;
  int get() const { return v + 100; }
};
} // namespace ns

int main(int argc, char **argv) {
  ns_ns_box a;
  a.v = argc;
  ns::box b;
  b.v = argc;
  printf("%d %d\n", a.get(), b.get());
  return 0;
}

//--- prefix-collide-reverse.cpp
// The mirror order for the prefix-induced clash.
// PREFIXREV: prefix-collide-reverse.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct 'ns_ns_box' collides with the emitted name of a different struct in this translation unit
extern "C" int printf(const char *, ...);
namespace ns {
struct box {
  int v;
  int get() const { return v + 100; }
};
} // namespace ns

struct ns_ns_box {
  int v;
  int get() const { return v; }
};

int main(int argc, char **argv) {
  ns::box b;
  b.v = argc;
  ns_ns_box a;
  a.v = argc;
  printf("%d %d\n", a.get(), b.get());
  return 0;
}

// Rename OFF is the AST-pure injective spelling, and it still transpiles
// the clashing pair: two structs, two method sets, no diagnostic. This
// is why the guard keys off the EMITTED name and not off name reuse.
// PRESERVED-DAG: emitrust.struct_def @box_i32 ["v"] [i32]
// PRESERVED-DAG: emitrust.struct_def @BoxI32 ["v"] [i32]
// PRESERVED-DAG: func.func @box_i32_get
// PRESERVED-DAG: func.func @BoxI32_get

// The FR-42 recovery half: the clash recovers like any other located
// rejection — the second record is DROPPED (never merged), the function
// that names it takes a loud `unimplemented!()` stub, and the item is
// tabulated under its own ledger tag rather than the catch-all `other`.
// `record-name-clash` is language-agnostic (a plain C program reaches it
// too), and it is mirrored in test/RealWorld/run_realworld.py.
// RENAMERECDIAG: rename-collide.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: struct 'BoxI32' collides with the emitted name of a different struct in this translation unit (recovered: item dropped)
// RENAMERECDIAG: dropped 'BoxI32' [record-name-clash] unsupported: struct 'BoxI32' collides with the emitted name of a different struct in this translation unit
// RENAMERECDIAG: stubbed 'c_main' [rejected-type-cascade]

// The recovered crate keeps the FIRST record (which legitimately owns the
// name) and everything of the second is gone: no second struct, no second
// method body — its `v + 7` must not reach emitted Rust under any mode,
// which is what the whole-crate `--implicit-check-not` on the RUN line
// scans for. The function that named the dropped type is a LOUD stub, not
// a silently re-bound call to the surviving type's `get`.
// RENAMEREC: struct BoxI32 {
// RENAMEREC: fn c_main
// RENAMEREC: unimplemented!("unsupported: struct 'BoxI32' was rejected, so a type naming it cannot be imported")
// RENAMEREC: impl BoxI32 {
// RENAMEREC: fn get(&self) -> i32 {
// RENAMEREC: self.v + 100i32
// RENAMEREC-NOT: struct BoxI32 {
