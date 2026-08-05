// FR-61f: a canonical ascending C counting loop `for (int i = LO; i < HI;
// i += K)` lifts to an `emitrust.for` range head (`for i in LO..HI`) at
// import. This pins BOTH halves with the byte-diff oracle: the emitted crate's
// stdout must match the clang-native binary exactly (the lift is behaviour-
// neutral), and the greps pin that the lift actually fires (a range `for`, the
// induction used directly, an accumulator folded to `+=`) and that the
// deliberately non-canonical shapes fall back to the CFG `while`.
// REQUIRES: cargo
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/range_for > %t.emit.out
// RUN: diff %t.native.out %t.emit.out
//
// The accumulator loop fires: range head + induction used directly + the
// `s = s + i` self-assign folded to `s += i`; its constant init rides the
// place's init attribute (no late `let mut s; s = 0`).
// RUN: grep "for i in 0i32..n" %t.crate/src/main.rs
// RUN: grep "s += i" %t.crate/src/main.rs
// RUN: grep "let mut s: i32 = 0" %t.crate/src/main.rs
// The array-fill loop fires (induction feeds the subscript directly).
// RUN: grep "for i in 0i32..5i32" %t.crate/src/main.rs
// A non-unit step keeps `.step_by`.
// RUN: grep "step_by(2i32 as usize)" %t.crate/src/main.rs
// The `i <= hi` inclusive loop is NOT canonical (v1 half-open only): it must
// fall back to a `while`, never a range `for`.
// RUN: grep "while " %t.crate/src/main.rs

int printf(const char *, ...);

// Accumulator: `s` is touched inside the lifted body, so it materializes as a
// mutable place; `i` is the range induction used directly.
int sum_to(int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s = s + i;
  return s;
}

// Non-unit step.
int evens_below(int n) {
  int s = 0;
  for (int i = 0; i < n; i += 2)
    s = s + i;
  return s;
}

// Inclusive bound: not canonical in v1, stays a `while`.
int inclusive_sum(int n) {
  int s = 0;
  for (int i = 1; i <= n; i++)
    s = s + i;
  return s;
}

int main(void) {
  int a[5];
  for (int i = 0; i < 5; i++)
    a[i] = i * i;
  int fill = 0;
  for (int i = 0; i < 5; i++)
    fill = fill + a[i];
  printf("%d %d %d %d\n", sum_to(10), evens_below(10), inclusive_sum(4), fill);
  return 0;
}
