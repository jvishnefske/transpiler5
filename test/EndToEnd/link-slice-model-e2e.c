// REQUIRES: cargo
// FR-158 phase 1, the link-time SLICE-MODEL RECONCILIATION, end to end.
//
// The invariant pinned here: when a body-less declaration in one shard and
// the definition in another disagree ONLY by `!emitrust.mut_ref<T>` versus
// `!emitrust.mut_ref<!emitrust.slice<T>>` on some parameters -- the shape
// `collectSliceParams` produces because a declaration has no body to
// promote from -- the merge REWRITES the declaring shard's `&arr[k]`
// arguments into the equivalent `&mut arr[k..]` instead of dropping the
// declaration and handing rustc an E0308 with no source location. This is
// the whole of the 501-object systemd link failure (measured: 6538 of 6542
// rustc errors were E0308, and 3796 of 3796 diverging declarations had
// exactly this shape).
//
// The rewrite is CODEGEN -- `&mut a[k]` and `&mut a[k..]` both compile, and
// a wrong base or a wrong index would read different memory while building
// clean -- so `cargo build` is not the oracle. The oracle is the stdout
// byte-diff against the clang-built native, with every index derived from
// `argc` so no constant fold can hide a miscompile; the three RUN lines
// walk k over 0, 1 and 2.
//
// The shapes covered, all in one crate:
//   * a RUNTIME index over a local array (`emit(&buf[k], 3)`);
//   * a NON-i8 element type (`isum(&nums[k], 2)`, `&mut [i32]`);
//   * a FORWARDED-PARAMETER base -- `fwd`'s own `p` is itself a slice, so
//     the reconciled argument is built over a deref of a reference
//     parameter, not over a local array;
//   * a SECOND call sharing a base (`emit(&buf[0], 2)`);
//   * a SHARED BASE AND INDEX feeding two callees of which only one is
//     slice-refined (`isum(&nums[k], 2) + peek(&buf[k])`): `peek` stays a
//     scalar reference in BOTH shards, so its borrow must survive
//     untouched -- pinned by the RUST FileCheck below;
//   * `fwd` itself, declared scalar in the main TU and defined
//     slice-classified in the use TU, so the reconciliation also runs in
//     the shard that owns `main`.
//
// The per-TU `-D` is LOAD-BEARING, not decoration. FR-58's remedy for a
// signature-starved declaration is a joint link-time RE-IMPORT of the
// group's sources, which reconciles the two models in the importer; it is
// vetoed when the members' recorded import args differ (measured on the
// systemd link: `link-time re-import:` 0 occurrences, `cannot re-import` 1).
// Without a distinct `-D` per TU the re-import rescues this program and the
// merge-level path under test never runs.
//
// Per-TU shards through the shim, each with its own import args:
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -DTU_MAIN=1 -o %t.main.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-slice-model-def.c -DTU_DEF=1 -o %t.def.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-slice-model-use.c -DTU_USE=1 -o %t.use.o
//
// The re-import really is vetoed, so the merge really is what reconciles:
// RUN: emitrust-cc --link %t.main.o %t.def.o %t.use.o --emit=rust -o %t.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=NOREIMPORT
// NOREIMPORT: cannot re-import fact-starved group
// NOREIMPORT-NOT: link-time re-import:
//
// RUN: emitrust-cc --link %t.main.o %t.def.o %t.use.o -o %t.crate --crate-name link_slice_model --build
//
// Differential oracle against the clang-linked native binary, k = 0, 1, 2:
// RUN: clang -std=c11 %s %S/Inputs/link-slice-model-def.c %S/Inputs/link-slice-model-use.c -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/link_slice_model > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/link_slice_model a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b > %t.n2.out && %t.crate/target/release/link_slice_model a b > %t.r2.out
// RUN: diff %t.n2.out %t.r2.out
//
// The mechanism in the emitted Rust: the slice-refined arguments carry the
// tail-borrow form, and `peek`'s scalar borrow of the SAME base and index
// does not.
// RUN: FileCheck %s --check-prefix=RUST < %t.crate/src/main.rs

int printf(const char *, ...);
int run(int k);
int fwd(char *p, int k);

int main(int argc, char **argv) {
  char t[6] = "wxyz!";
  printf("sum=%d\n", run(argc - 1));
  fwd(t, argc - 1);
  return 0;
}

// RUST-DAG: &mut [i8] = &mut buf[k as usize..]
// RUST-DAG: &mut [i32] = &mut nums[k as usize..]
// RUST-DAG: &mut i8 = &mut buf[k as usize]{{;$}}
// RUST-DAG: &mut [i8] = &mut t[0i64 as usize..]
