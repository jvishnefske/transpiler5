// REQUIRES: cargo
// FR-153 fence R1, differentially: a cursor argument whose region is a
// STRING LITERAL's read-only backing, handed to a callee whose cursor
// base slot is MUTABLE.
//
// The ordinary mutable-slice-argument path already rematerializes a fresh
// non-const backing for exactly this case (writing through a pointer to a
// string literal is undefined behavior in C, so the per-call copy is
// unobservable to any defined program). The cursor-argument path never
// got it, so once the base slot could be mutable the caller reborrowed
// the shared `const` literal backing mutably and the emitted crate died
// with `error[E0596]: cannot borrow ... as mutable`.
//
// This pins the runtime half: the per-call copy must not change what the
// program prints. Stdout is byte-diffed against the clang-built native
// binary; seeds derive from argc so neither the cursor offset nor the
// bytes the callee reads can constant-fold.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name cursor_region_demand_mut_literal --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cursor_region_demand_mut_literal > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

// A NON-const `char *` parameter: unambiguously a mutable byte slice.
static int peek2(char *s) { return (int)s[0] + (int)s[1]; }

static int step2(char **p) {
  if (*p)
    return peek2(*p);
  return -1;
}

int main(int argc, char **argv) {
  /* A `char *` bound to a string literal: the region is the literal's
     read-only backing, which has no VarDecl base. */
  char *s = "ABC";
  s = s + (argc - 1);
  int v = step2(&s);
  printf("v=%d c=%c\n", v, *s);
  return 0;
}
