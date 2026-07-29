// FR-50/FR-41: a Red record poisons the record that embeds it BY VALUE, and
// does NOT poison one that only points at it.
//
// This is the under-approximation contract made concrete. `ItemColoring.h`
// requires that the probe never call an IMPORTABLE item Red, because FR-43's
// search starts from the Green-or-Yellow set and every child admits strictly
// fewer items -- a false Red is an item the port loses with no diagnostic and
// no way back. A false Green costs one import attempt. The two errors are not
// comparable, so every judgement call resolves toward Green.
//
// The verdicts below are MEASURED, not argued. With `struct Atom` rejected
// (`_Atomic` is one of `mapType`'s two unconditional type rejections):
//
//   struct Holder { struct Atom a; }  emits `{ a: Atom }`   -> does NOT
//                                     compile once Atom is dropped. RED.
//   struct Cursor { struct Atom *p; } emits `{ p: i64 }`    -> compiles.
//                                     GREEN, and porting it is a real item
//                                     the old rule threw away.
//
// RUN: emitrust-cc --emit=coloring %s -o - | FileCheck %s
//
// The second half of the claim: the crate the coloring describes is the crate
// the importer actually produces. Without this, "Cursor is Green" would be a
// statement about the analysis rather than about the port. (That the crate
// also COMPILES is pinned, with cargo, in search-false-red.cpp.)
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/main.rs

struct Atom {
  _Atomic int cell;
};

// By value: the field list names `Atom`, which will not exist.
struct Holder {
  struct Atom a;
  int k;
};

// Through a pointer: the member is erased and `Atom` is never named.
struct Cursor {
  struct Atom *p;
  int k;
};

int use(int x) { return x + 1; }

int main(void) { return use(1); }

// CHECK:      item Atom kind=record color=red reason=inadmissible construct=atomic-type
// CHECK-NEXT: item Cursor kind=record color=green reason=admissible

// Not Yellow either. Yellow means "emits, but calls a stub", and a record
// calls nothing -- so a `FieldIndirect` edge into a Red target carries no
// poison in either direction.
// CHECK-NEXT: item Holder kind=record color=red reason=red-type via=Atom edge=Field chain=Holder->Atom construct=atomic-type
// CHECK-NEXT: item c_main kind=function color=green reason=admissible
// CHECK-NEXT: item use_ kind=function color=green reason=admissible
// CHECK-NEXT: tally green=3 yellow=0 red=2

// `Cursor` really is in the crate, with its pointer member erased to an
// integer; `Holder` and `Atom` really are not.
// RUST:      struct Cursor {
// RUST-NEXT:     p: i64,
// RUST-NEXT:     k: i32,
// RUST-NEXT: }
// RUST-NOT:  struct Holder
// RUST-NOT:  struct Atom
