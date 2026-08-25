// FR-61f-5: the ASSIGNMENT-FORM init `for (i = LO; i < HI; i += K)`, where the
// induction is declared OUTSIDE the loop. Until now only the `for (int i = ..)`
// DeclStmt form lifted, because an init-declared induction is loop-scoped in C
// and so "nothing reads it after the loop" held for free. Rust's `for i in
// LO..HI` binds `i` loop-scoped too, so the assignment form is only sound when
// the induction is provably DEAD AFTER the loop -- otherwise the value C would
// have observed afterwards is silently dropped.
//
// The obligation is ONE-DIRECTIONAL, which is why the analysis refuses by
// default: a wrong "dead" answer silently drops a value (no rustc diagnostic
// can see it), while a wrong "live" answer merely keeps today's `while`. The
// exit value is deliberately NOT written back -- for step K it is
// LO + ceil((HI-LO)/K)*K, not HI, and it is only defined if the loop ran to
// completion.
//
// Every case below is byte-diffed against the clang-built native at several
// argument values, so nothing constant-folds and a dropped induction shows up
// as a wrong number rather than a compile error.
//
// REQUIRES: cargo
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native 0 > %t.n0.out && %t.crate/target/release/range_for_assign_init 0 > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/range_for_assign_init a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/range_for_assign_init a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out
//
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s

int printf(const char *, ...);

// LIFTS: `i` is never read after the loop, so the loop-scoped Rust binding is
// unobservable.
// CHECK-LABEL: fn dead_after
// CHECK:         for {{i|_i}} in 0i32..
// CHECK-NOT:     while
int dead_after(int n) {
  int s = 0, i;
  for (i = 0; i < n; i++)
    s += i;
  return s;
}

// REFUSES: `i` IS read after the loop. Must stay a `while`, or the returned
// value is wrong.
// CHECK-LABEL: fn read_after
// CHECK:         while
int read_after(int n) {
  int s = 0, i;
  for (i = 0; i < n; i++)
    s += i;
  return s + i;
}

// LIFTS TWICE: the single most common real shape -- one induction reused by a
// later loop. The second loop's `i = 0` is a pure DEFINITION, not a read, so
// `i` is dead after BOTH loops and both lift.
// CHECK-LABEL: fn reused_by_later_loop
// CHECK:         for {{i|_i}} in 0i32..
// CHECK:         for {{i|_i}}{{_?[0-9]*}} in 0i32..
// CHECK-NOT:     while
int reused_by_later_loop(int n) {
  int s = 0, i;
  for (i = 0; i < n; i++)
    s += i;
  for (i = 0; i < n; i++)
    s += i * 2;
  return s;
}

// REFUSES -- the regression pin for a MEASURED MISCOMPILE. An early `return`
// sits between the loop and a later redefinition of `i`. A sibling scan that
// stops at the control transfer concludes "a redefinition follows, so `i` is
// dead" and lifts; but on the path that does NOT take the early return, the
// read below observes the loop's exit value. The spike measured this printing
// 303 where clang prints 300 -- and the crate COMPILED CLEAN, so only the
// byte-diff caught it. A control transfer must therefore stop the scan from
// TRUSTING later kills while still counting later reads.
// CHECK-LABEL: fn early_exit_then_read
// CHECK:         while
int early_exit_then_read(int n, int c) {
  int s = 0, i;
  for (i = 0; i < n; i++)
    s += i;
  if (c)
    return s;
  return s * 100 + i;
}

// LIFTS -- the regression pin for the second defect. `i` is redefined and then
// read, which is legal and must lift; but `emitRangeFor` registered the
// induction's region-local SSA value and never unregistered it, so the read
// below picked up a value defined inside the loop region and MLIR failed with
// "operand does not dominate this use". With the DeclStmt form the VarDecl
// died with the loop and the stale entry was unreachable; the assignment form
// is what exposes it. The fix saves and restores the mapping around the body.
// CHECK-LABEL: fn kill_then_read
// CHECK:         for {{i|_i}} in 0i32..
int kill_then_read(int n) {
  int s = 0, i;
  for (i = 0; i < n; i++)
    s += i;
  i = 5;
  return s * 100 + i;
}

// REFUSES: `&i` escapes. Existing clause 4 covers this; pinned so a later
// widening cannot quietly drop it. The refusal is what matters, not which
// fallback spelling it takes -- taking `&i` forces the induction onto a place,
// which rotates the emitted loop to `loop { .. }` rather than `while`.
// CHECK-LABEL: fn address_taken
// CHECK:         {{while |loop \{}}
int address_taken(int n) {
  int s = 0, i;
  for (i = 0; i < n; i++)
    s += i;
  int *p = &i;
  return s + *p;
}

int main(int argc, char **argv) {
  int n = argc;
  printf("%d %d %d %d %d %d %d\n", dead_after(n), read_after(n),
         reused_by_later_loop(n), early_exit_then_read(n, 0),
         early_exit_then_read(n, 1), kill_then_read(n), address_taken(n));
  return 0;
}
