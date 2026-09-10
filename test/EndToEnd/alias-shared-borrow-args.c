// REQUIRES: cargo
// FR-222: two SHARED borrows of one object are sound in Rust — that is the
// whole point of `&` — and `emitCall`'s aliasing collision key used to
// ignore borrow mutability, so this whole file was a LOCATED REJECTION
// ("aliasing mutable pointer arguments"), a refusal of a provably correct
// program whose diagnostic word "mutable" was factually false for the
// shape. This test is the correctness oracle for admitting it: compile-only
// evidence cannot see a miscompile, so the pin is the stdout byte-diff
// against the clang-built native.
//
// Every callee here is READ-ONLY over both of its `const unsigned char *`
// parameters, which map to `&[u8]`, and every cursor pair rides the SAME
// region: disjoint constant offsets, RUNTIME offsets, and the degenerate
// identical-pointer pair. If the emitted reslice starts at the wrong index
// — an off-by-one cursor, a swapped pair, a window that silently narrows —
// the sums below change and the diff catches it. `p` arrives as a
// PARAMETER on purpose: the Phase-4 owner lift is gated to caller-LOCAL
// regions (`planOwners`), so a parameter-rooted region cannot be promoted
// into i64 indices, and the arguments stay real borrows that must go
// through the aliasing key. The buffer likewise lives in a STRUCT FIELD so
// the lift cannot promote it out from under the call.
//
// All bytes derive from argc so constant folding cannot pre-compute a
// buffer and hide a miscompile; the second RUN pair re-seeds via extra
// argv words. Deterministic, no UB — every index below is in bounds for
// both seeds.
//
// The rejected side of this frontier — a mutable borrow on EITHER side —
// is pinned next door in test/Import/C/alias-shared-borrow-args-invalid.c
// and must be read with this file: admitting the shared pair without
// keeping the mutable pairs refused would turn `f(p, p)` over `u8 *` into
// a rustc E0499 crate, a build failure instead of a diagnostic.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/alias_shared_borrow_args > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/alias_shared_borrow_args a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>

typedef unsigned char u8;

/* Read-only over BOTH parameters: `const unsigned char *` maps to the
   shared byte slice `&[u8]`, so neither borrow is mutable. */
static int mix(const u8 *a, const u8 *b) {
  int s = 0;
  int i;
  for (i = 0; i < 4; i++)
    s += (int)a[i] * (i + 1) - (int)b[i];
  return s;
}

/* Disjoint CONSTANT cursors into one parameter-rooted region. */
static int const_cursors(u8 *p) { return mix(p, p + 4); }

/* RUNTIME cursors: nothing static proves them disjoint, and they need not
   be — two shared borrows may overlap. */
static int runtime_cursors(u8 *p, int i, int j) { return mix(p + i, p + j); }

/* The degenerate pair: the SAME pointer twice. Still two shared borrows. */
static int same_pointer(u8 *p) { return mix(p, p); }

/* Overlapping-but-not-equal cursors, the shape a disjointness test would
   wrongly refuse. */
static int overlapping_cursors(u8 *p) { return mix(p + 1, p + 2); }

struct Box {
  u8 buf[16];
  int tag;
};

int main(int argc, char **argv) {
  struct Box s;
  int seed = argc;
  int i;
  for (i = 0; i < 16; i++)
    s.buf[i] = (u8)(i * 7 + seed * 13);
  s.tag = seed * 3;
  printf("const %d\n", const_cursors(s.buf));
  printf("runtime %d\n", runtime_cursors(s.buf, seed, seed + 4));
  printf("same %d\n", same_pointer(s.buf));
  printf("overlap %d\n", overlapping_cursors(s.buf));
  /* The region is untouched by any of the calls above: a shared borrow
     cannot write, so a lowering that quietly staged a mutable copy back
     would show up right here. */
  printf("buf %d %d %d\n", (int)s.buf[0], (int)s.buf[15], s.tag);
  return 0;
}
