// REQUIRES: cargo
// FR-195, the FOURTH collision channel, found while closing the other
// three: the per-call-site va_list monomorphization clone name.
//
// `Total` and `total` are two distinct C identifiers that the FR-53
// idiomatic rename folds onto one emitted symbol. A monomorphized variadic
// definition never emits that bare symbol -- only its per-signature clones
// -- but the clones were NAMED from it, and each plan uniquified only
// against the module and the TU's ordinary names, so both definitions chose
// `tu0_total_1`. Emission then refused the second one ("monomorphization
// clone name ... collides with an existing symbol"); under `--incremental`
// that dropped the loser while every `total(...)` call, which resolves
// through the clone NAME, silently executed `Total`'s body: native
// `3 1003`, emitted `3 3`, exit 0, clean cargo build.
//
// The clones are the ONLY names that exist for these two definitions, so
// making them mutually unique is all the fold needs -- both definitions
// import, and this is a byte-diff of the whole program rather than a
// rejection pin. Seeds derive from argc so constant folding cannot
// pre-compute the sums.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build \
// RUN:   --crate-name varargs_fold_clone_names
// RUN: clang %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/varargs_fold_clone_names > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// The two definitions really are two items, and the folded bare symbol is
// emitted for neither.
// RUN: FileCheck %s --input-file=%t.crate/src/main.rs

#include <stdarg.h>

int printf(const char *, ...);

static int Total(int n, ...) {
  va_list ap;
  int t = 0;
  va_start(ap, n);
  for (int i = 0; i < n; ++i)
    t += va_arg(ap, int);
  va_end(ap);
  return t;
}

static int total(int n, ...) {
  va_list ap;
  int t = 1000;
  va_start(ap, n);
  for (int i = 0; i < n; ++i)
    t += va_arg(ap, int);
  va_end(ap);
  return t;
}

int main(int argc, char **argv) {
  int s = argc; // 1 on a bare run, but the compiler cannot know that.
  printf("upper=%d\n", Total(2, s, s + 4));
  printf("lower=%d\n", total(2, s, s + 4));
  printf("mixed=%d\n", Total(2, s, s + 1) + total(2, s, s + 2));
  return 0;
}

// CHECK-DAG: fn tu0_total_1(
// CHECK-DAG: fn tu0_total_2(
// CHECK-NOT: fn tu0_total(
