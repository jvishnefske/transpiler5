// FR-61f-f: INDUCTION REUSE ACROSS A SECOND NESTED PAIR. Two nested `i`/`j`
// loop pairs in a row, the second reusing the same two induction variables as
// the first, is the single most common shape in array code -- and until now
// the FIRST pair did not lift. The `i` half already worked (the second pair's
// `for (i = 0; ..)` init is a pure definition, so `i` is dead after the first
// pair); the `j` half did not, because the kill of `j` lives one level deeper,
// in the INNER loop's init, and the whole second outer `for` was therefore
// classified as a READ of `j`. The inner loop refusing dragged the outer one
// down with it (a non-lifting nested loop blocks its parent), so a pair that
// should have emitted two range heads emitted two `loop { .. }` CFG lowerings.
//
// The widening is a CODEGEN change and the failure direction is a MISCOMPILE:
// a wrong "dead" answer drops the value C would have observed after the loop,
// which no rustc diagnostic can see. So this file is byte-diffed against the
// clang-built native at three argument counts, and every case below is a value
// that CHANGES if the incoming induction is dropped. The three refusal cases
// are the load-bearing half -- they pin the exact edge of the widening, so a
// later attempt to relax it further has to break a test to do it.
//
// REQUIRES: cargo
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native 0 > %t.n0.out && %t.crate/target/release/range_for_nested_reuse 0 > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/range_for_nested_reuse a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/range_for_nested_reuse a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out
//
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s

int printf(const char *, ...);

int ga[8][8];
int gb[8][8];
int gc[8][8];
int gd[8][8];

// LIFTS FOUR TIMES -- the FR-61f-f goal. Nothing between or after the two
// pairs reads `i` or `j`, and the second pair redefines both before reading
// them, so both loop-scoped Rust bindings are unobservable.
// CHECK-LABEL: fn nested_reuse
// CHECK-NOT:     {{while |loop \{}}
// CHECK:         for {{_?i}} in 0i32..8i32
// CHECK:         for {{_?j}} in 0i32..8i32
// CHECK:         for {{_?i}}{{_?[0-9]*}} in 0i32..8i32
// CHECK:         for {{_?j}}{{_?[0-9]*}} in 0i32..8i32
// CHECK-NOT:     {{while |loop \{}}
int nested_reuse(int n) {
  int s = 0, i = 0, j = 0;
  for (i = 0; i < 8; i++)
    for (j = 0; j < 8; j++)
      ga[i][j] = i * n + j;
  for (i = 0; i < 8; i++)
    for (j = 0; j < 8; j++)
      s += ga[i][j];
  return s;
}

// REFUSES -- THE OVER-RELAXATION GUARD. Identical to `nested_reuse` except
// that a genuine read of BOTH inductions sits between the two pairs. The exit
// values (`i == 8`, `j == 8`) are observed there, so the first pair may not
// take loop-scoped bindings. If a later widening classifies the read-between
// away, this is what catches it -- in the emitted shape, not just at runtime.
// CHECK-LABEL: fn nested_read_between
// CHECK-NOT:     for {{_?[ij]}} in 0i32..
// CHECK:         {{while |loop \{}}
// CHECK-NOT:     for {{_?[ij]}} in 0i32..
// CHECK:         {{while |loop \{}}
int nested_read_between(int n) {
  int s = 0, i = 0, j = 0;
  for (i = 0; i < 8; i++)
    for (j = 0; j < 8; j++)
      gb[i][j] = i * n + j;
  s += i + j;
  for (i = 0; i < 8; i++)
    for (j = 0; j < 8; j++)
      s += gb[i][j];
  return s;
}

// REFUSES -- a `while` has NO INIT. The second construct kills `j` in exactly
// the same place the lifting case does (the nested `for (j = 0; ..)` init),
// but a `while` body is not guaranteed to run and, unlike a `for`, offers no
// once-on-entry slot that dominates its own condition. It is not modelled, so
// it stays a READ of `j` and the first pair keeps its CFG lowering.
// CHECK-LABEL: fn nested_while
// CHECK-NOT:     for {{_?[ij]}} in 0i32..
// CHECK:         {{while |loop \{}}
// CHECK-NOT:     for {{_?[ij]}} in 0i32..
// CHECK:         {{while |loop \{}}
int nested_while(int n) {
  int s = 0, i = 0, j = 0, k = 0;
  for (i = 0; i < 8; i++)
    for (j = 0; j < 8; j++)
      gc[i][j] = i * n + j;
  while (k < 8) {
    for (j = 0; j < 8; j++)
      s += gc[k][j];
    k++;
  }
  return s;
}

// REFUSES -- a COMPOUND init reads before it writes. `j -= 4` is not a whole-
// variable definition of `j`; it consumes the 8 the first pair left behind, so
// the second pair's column range depends on it. Dropping the incoming value
// would change the sum, which is why this is byte-diffed and not merely
// shape-checked.
// CHECK-LABEL: fn nested_compound_init
// CHECK-NOT:     for {{_?[ij]}} in 0i32..
// CHECK:         {{while |loop \{}}
// CHECK-NOT:     for {{_?[ij]}} in 0i32..
// CHECK:         {{while |loop \{}}
// CHECK-NOT:     for {{_?[ij]}} in 0i32..
int nested_compound_init(int n) {
  int s = 0, i = 0, j = 0;
  for (i = 0; i < 8; i++)
    for (j = 0; j < 8; j++)
      gd[i][j] = i * n + j;
  for (i = 0; i < 8; i++)
    for (j -= 4; j < 8; j++)
      s += gd[i][j];
  return s;
}

// REFUSES -- the pin for the most dangerous property of this widening. The
// middle construct does NOT observe the incoming `i` (its nested `for (i = 0;
// ..)` init dominates every read of `i` inside it), but it is NOT a kill of
// `i` either: at argc == 1 the `k` loop runs zero times and `i` keeps the 8
// the first loop left. The `return` below reads it. So "does not observe"
// must merely CONTINUE the sibling scan, never end it with "dead" -- the same
// distinction the `early_exit_then_read` miscompile in range-for-assign-init.c
// was measured on. Byte-diffed at argc 1, where the value differs.
// CHECK-LABEL: fn pass_is_not_a_kill
// CHECK-NOT:     for {{_?[ik]}} in 0i32..
// CHECK:         {{while |loop \{}}
// CHECK-NOT:     for {{_?[ik]}} in 0i32..
// CHECK:         {{while |loop \{}}
// CHECK-NOT:     for {{_?[ik]}} in 0i32..
int pass_is_not_a_kill(int n) {
  int s = 0, i = 0, k = 0;
  for (i = 0; i < 8; i++)
    s += i;
  for (k = 0; k < n - 1; k++) {
    for (i = 0; i < 3; i++)
      s += i;
  }
  return s * 100 + i;
}

// REFUSES -- ORDER MATTERS INSIDE THE SECOND CONSTRUCT. Identical to
// `pass_is_not_a_kill` except the read of `i` sits BEFORE the nested
// redefinition, so on the first `k` iteration it observes the first loop's
// exit value. A scan that only asked "is there a redefinition somewhere in
// here" instead of "is every read dominated by one" would lift this and print
// a smaller number.
// CHECK-LABEL: fn read_before_nested_kill
// CHECK-NOT:     for {{_?[ik]}} in 0i32..
// CHECK:         {{while |loop \{}}
// CHECK-NOT:     for {{_?[ik]}} in 0i32..
// CHECK:         {{while |loop \{}}
// CHECK-NOT:     for {{_?[ik]}} in 0i32..
// CHECK-LABEL: fn c_main
int read_before_nested_kill(int n) {
  int s = 0, i = 0, k = 0;
  for (i = 0; i < 8; i++)
    s += i;
  for (k = 0; k < n; k++) {
    s += i;
    for (i = 0; i < 3; i++)
      s += i;
  }
  return s;
}

int main(int argc, char **argv) {
  int n = argc;
  printf("%d %d %d %d %d %d\n", nested_reuse(n), nested_read_between(n),
         nested_while(n), nested_compound_init(n), pass_is_not_a_kill(n),
         read_before_nested_kill(n));
  return 0;
}
