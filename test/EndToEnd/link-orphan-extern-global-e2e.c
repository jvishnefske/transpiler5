// REQUIRES: cargo
// FR-168 slice 1, the two-TU shape this actually blocks, reproduced from
// systemd's `vtable`/`sym-table` idiom: a flexible-array-member interface
// record whose DEFINITION is dropped (`unsupported: non-constant global
// initializer` -- the pointer members), read from another TU inside a body
// that is itself rejected LATER (inline asm, after the read was imported).
//
// The read registers a `pendingExternGlobals` entry; `rollbackTo` does not
// undo it; `finalizeProject` used to materialize an
// `emitrust.global {emitrust.extern_decl}` obligation for a symbol that
// NOTHING in the merged program references, and the FR-58 link step then
// refused the whole crate with "unresolved external 'VL_IFACE' at link".
// At HEAD this pair could not produce a crate at all.
//
// `peek` is stubbed and never called; `report` carries the observable
// value, and its seed derives from `argc` so no constant fold can stand in
// for the argument. --release matches the other EndToEnd tests.
//
// Per-TU shards through the FR-56 shim, exactly as a real build produces
// them (the defect only exists in defer mode):
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-orphan-extern-global-def.c -o %t.def.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
//
// The link must succeed and build:
// RUN: emitrust-cc --link %t.def.o %t.main.o -o %t.crate --crate-name link_orphan_extern_global --build
//
// The orphaned obligation must be gone from the merged crate -- not merely
// tolerated by the link:
// RUN: FileCheck %s --check-prefix=NOOBLIG < %t.crate/src/main.rs
// NOOBLIG-NOT: VL_IFACE
//
// Differential oracle against the clang-linked native binary, seed = 7*argc
// over three argument counts:
// RUN: clang -std=c11 %S/Inputs/link-orphan-extern-global-def.c %s -o %t.native
// RUN: %t.native > %t.n0.out
// RUN: %t.crate/target/release/link_orphan_extern_global > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out
// RUN: %t.crate/target/release/link_orphan_extern_global a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b > %t.n2.out
// RUN: %t.crate/target/release/link_orphan_extern_global a b > %t.r2.out
// RUN: diff %t.n2.out %t.r2.out

struct Sym { int tag; };
struct Iface { int n; const struct Sym *syms[]; };
extern const struct Iface vl_iface;
int report(int seed);
int printf(const char *, ...);

int peek(int n) {
  int r = vl_iface.n + n;
  __asm__ volatile ("nop"); /* rejected AFTER the read is imported */
  return r;
}

int main(int argc, char **argv) {
  printf("report=%d\n", report(7 * argc));
  return 0;
}
