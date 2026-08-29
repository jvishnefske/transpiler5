// REQUIRES: cargo
// FR-150 x FR-133 INTERACTION, pinned explicitly and byte-diffed.
//
// FR-133 (landed one day before FR-150) drops the `Option` wrapper for a
// fn-ptr LOCAL that is initialized once from a literal `Some` and only ever
// CALLED. That PARTIALLY MASKS the FR-150 collision: a repro written entirely
// out of such locals compiles clean beside a `struct Option { .. }` today,
// because no `Option<..>` is written at all. Two consequences this file pins:
//
//   1. THE MASK MUST SURVIVE. A crate that shadows `Option` and contains an
//      FR-133-admitted local must still emit the unwrapped `let cp: fn(i32) ->
//      i32 = tu0_inc;` and the bare `cp(x)` call -- qualification may not leak
//      into a spelling the wrapper was already dropped from. The emitted shape
//      is pinned line-for-line in test/Target/Rust/prelude-shadow.mlir
//      (@fr133_local); this file pins that it still RUNS correctly, which the
//      shape alone cannot say.
//   2. THE REFUSAL LEGS STILL COLLIDE. FR-133 refuses to unwrap a local that
//      is reassigned, compared against null, or escapes; those keep their
//      `Option<fn(..)>` and are exactly the locals FR-150 has to qualify. Both
//      kinds live side by side below, in one crate, so a fix that qualified
//      only non-local positions would fail here.
//
// Because two spellings of the same C construct now coexist, `cargo build`
// proves even less than usual: unwrapping the wrong one, or unwrapping one and
// then calling through the other, compiles. Every value derives from argc,
// three argument counts are run, and the crate's stdout is diffed against the
// clang-built native.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native0.out
// RUN: %t.crate/target/release/prelude_shadow_fnptr_local > %t.rust0.out
// RUN: diff %t.native0.out %t.rust0.out
// RUN: %t.native one > %t.native1.out
// RUN: %t.crate/target/release/prelude_shadow_fnptr_local one > %t.rust1.out
// RUN: diff %t.native1.out %t.rust1.out
// RUN: %t.native one two three > %t.native3.out
// RUN: %t.crate/target/release/prelude_shadow_fnptr_local one two three > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>

// The prelude collision.
typedef struct Option {
  int id;
} Option;

static int inc(int x) { return x + 1; }
static int dbl(int x) { return x * 2; }

// FR-133 ADMITTED: one literal `Some`, calls only. Emits no `Option` at all.
static int admitted(int n) {
  int (*cp)(int) = inc;
  return cp(n) + cp(n + 1);
}

// FR-133 REFUSAL LEG "reassigned": both stores are literal, so the wrapper
// survives and FR-150 must qualify it. If the fold ignored the second store
// the answer below would be wrong, not merely differently spelled.
static int reassigned(int n) {
  int (*cp)(int) = inc;
  int first = cp(n);
  cp = dbl;
  return first * 100 + cp(n);
}

// FR-133 REFUSAL LEG "null compared": the comparison is the whole reason the
// `Option` exists. The C program guards the call, so it is UB-free at every
// argc.
static int null_compared(int n, int pick) {
  int (*cp)(int) = 0;
  if (pick > 1)
    cp = dbl;
  if (cp == 0)
    return -n;
  return cp(n);
}

// FR-133 REFUSAL LEG "escapes": the local is passed as an argument, so its
// consuming position is typed `Option<fn(..)>`.
static int through(int (*h)(int), int v) { return h(v); }
static int escapes(int n) {
  int (*cp)(int) = dbl;
  return through(cp, n);
}

int main(int argc, char **argv) {
  Option o;
  o.id = argc * 4;

  printf("admitted=%d\n", admitted(o.id));
  printf("reassigned=%d\n", reassigned(argc));
  printf("null_compared=%d %d\n", null_compared(o.id, argc),
         null_compared(argc, 0));
  printf("escapes=%d\n", escapes(o.id + argc));
  printf("id=%d\n", o.id);
  return 0;
}
