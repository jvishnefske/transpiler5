// REQUIRES: cargo
// FR-161 phase 1, the ELEMENT-ZERO out-parameter across a link, end to end.
//
// The invariant pinned here: when a body-less declaration passes `&x` --
// the address of a scalar local or of a struct field -- and the defining
// shard classifies that parameter as a slice, the merge wraps the argument
// in `::std::slice::from_mut` and the callee's WRITES STILL LAND IN THE
// CALLER'S OBJECT. FR-158 phases 1+2 had no form for this shape (there is
// no region to tail-borrow) and left it a located rejection: 60 argument
// slots over 55 caller functions on the 501-object systemd link, all of
// them the C out-parameter idiom, and the last thing stopping that link
// from emitting a crate.
//
// `cargo build` cannot see this. `::std::slice::from_mut(&mut x)` and a
// wrap that lost the write-back -- a copy, a temporary, a re-borrow of the
// wrong place -- both compile. The oracle is the stdout byte-diff against
// the clang-built native, with every written value derived from `argc` so
// no constant fold can collapse the store.
//
// The shapes covered, all in one crate:
//   * a scalar LOCAL out-parameter (`setv(&x, ..)`), 50 of the 60 systemd
//     slots;
//   * a struct FIELD out-parameter (`setv(&s.a, ..)`), the other 10, with
//     the neighbouring field `s.b` as the witness that the one-element
//     view really is one element wide;
//   * a FORWARDED parameter (`outer` hands `&mut (*ret)[0..]` to `inner`),
//     the `parse_sec` -> `parse_time` shape that a non-transitive fence
//     would refuse -- after which nothing emits at all;
//   * an untouched neighbouring local (`guard`) that must survive the
//     borrows unchanged.
//
// The per-TU `-D` is LOAD-BEARING. FR-58's remedy for a signature-starved
// declaration is a joint link-time RE-IMPORT of the group's sources, which
// reconciles the two pointer models inside the importer; it is vetoed only
// when the members' recorded import args differ. Without a distinct `-D`
// per TU the re-import rescues this program and the merge-level path under
// test never runs.
//
// Per-TU shards through the shim, each with its own import args:
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -DTU_MAIN=1 -o %t.main.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-slice-out-param-def.c -DTU_DEF=1 -o %t.def.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-slice-out-param-use.c -DTU_USE=1 -o %t.use.o
//
// The re-import really is vetoed, so the merge really is what reconciles:
// RUN: emitrust-cc --link %t.main.o %t.def.o %t.use.o --emit=rust -o %t.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=NOREIMPORT
// NOREIMPORT: cannot re-import fact-starved group
// NOREIMPORT-NOT: link-time re-import:
//
// RUN: emitrust-cc --link %t.main.o %t.def.o %t.use.o -o %t.crate --crate-name link_slice_out_param --build
//
// Differential oracle against the clang-linked native binary, k = 0, 1, 2:
// RUN: clang -std=c11 %s %S/Inputs/link-slice-out-param-def.c %S/Inputs/link-slice-out-param-use.c -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/link_slice_out_param > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/link_slice_out_param a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b > %t.n2.out && %t.crate/target/release/link_slice_out_param a b > %t.r2.out
// RUN: diff %t.n2.out %t.r2.out
//
// The mechanism in the emitted Rust: the one-element view, spelled with the
// leading `::` and with `std`, never `core`.
// RUN: FileCheck %s --check-prefix=RUST < %t.crate/src/main.rs
// RUN: FileCheck %s --check-prefix=RUSTNOT < %t.crate/src/main.rs
// RUSTNOT-NOT: core::slice::from_mut
// RUSTNOT-NOT: = std::slice::from_mut

int printf(const char *, ...);
int run(int k);

int main(int argc, char **argv) {
  printf("rc=%d\n", run(argc - 1));
  return 0;
}

// RUST:      let [[X:v[0-9]+]]: &mut u32 = &mut x;
// RUST-NEXT: let {{v[0-9]+}}: &mut [u32] = ::std::slice::from_mut([[X]]);
// RUST-NEXT: = setv(
// RUST:      let [[A:v[0-9]+]]: &mut u32 = &mut s.a;
// RUST-NEXT: let {{v[0-9]+}}: &mut [u32] = ::std::slice::from_mut([[A]]);
// RUST-NEXT: = setv(
// RUST:      let [[Y:v[0-9]+]]: &mut u32 = &mut y;
// RUST-NEXT: let {{v[0-9]+}}: &mut [u32] = ::std::slice::from_mut([[Y]]);
// RUST-NEXT: = outer(
