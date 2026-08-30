// REQUIRES: cargo
// FR-153 differential: the cursor's region is a caller's SLICE PARAMETER,
// not a local array or a literal — the third region root the mutable
// cursor base has to reach, and an `error[E0596]` at HEAD like the other
// two.
//
// The chain here is two levels deep: `outer` receives a byte region as an
// ordinary slice parameter, opens a cursor local into it at an
// argc-derived offset, and passes that cursor to `step2`, which both
// forwards `*p` to a mutable-slice callee (the mutability demand) AND
// advances the cursor through `*p = *p + 1` (the write the caller must
// observe on return). Getting the base borrow right without getting the
// cursor writeback right would still print the wrong second column, so
// stdout is byte-diffed against the clang-built native binary rather than
// trusted to `cargo build`.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name cursor_region_demand_mut_param --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cursor_region_demand_mut_param > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

static int peek2(char *s) { return (int)s[0]; }

static int step2(char **p) {
  int v = -1;
  if (*p)
    v = peek2(*p);
  *p = *p + 1;
  return v;
}

static void outer(char *base, int seed) {
  char *c = base + seed;
  int v = step2(&c);
  printf("v=%d c=%d\n", v, (int)*c);
}

int main(int argc, char **argv) {
  char b[5] = {'x', 'y', 'z', 'w', 0};
  outer(b, argc - 1);
  return 0;
}
