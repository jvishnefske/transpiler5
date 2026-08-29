// FR-106: the `#[allow(unused_assignments)]` predicate is ALL-PATHS, and
// this test is the fence that keeps it there.
//
// The residual dead store FR-106 covers is real but RARE. Measured over
// 3475 emitted functions in 472 rustc-clean crates, with rustc 1.96.0 as
// the ground truth (it flags 4 of the 3475):
//     some-path, forward only : marks 127  -> 31.8 : 1 over-application
//     some-path + back edge   : marks 147  -> 36.8 : 1
//     ALL-PATHS               : marks   4  ->  1.00 : 1
// A some-path predicate would put the allow in 77+ crates, which is the
// crate-wide re-allow of `unused_assignments` in disguise -- it would
// delete the FR-53 tripwire that FOUND the heatshrink bug in the first
// place. Every function below is a shape a some-path predicate marks and
// rustc does NOT flag; if someone widens the predicate, this test fails.
//
// The four shapes, and which predicate each one catches:
//   `default_then_override`  x = a; if (c) x = b; use(x). FR-61b folds it
//       to an if-expression, so there is no store left at all -- pinned so
//       a future widening that re-introduces one cannot mark it either.
//   `store_then_break_then_read`  the store's value leaves through a
//       `break` and is read AFTER the loop. `analyzeSeq` merges break,
//       continue and return into ONE `diverges` bit, so the exit kinds have
//       to be re-derived; without that this is a false positive (it is
//       exactly `heatshrink_encoder_poll`'s shape).
//   `loop_carry`  `prev` is written at the bottom of the body and read at
//       the TOP of the next iteration: only following the BACK EDGE sees
//       the reader. A forward-only scan marks it.
//   `cond_read_after_loop`  a store on the `continue` path whose value is
//       read after the loop exits. Both the back edge and the post-loop
//       tail must be followed.
//
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s
// CHECK-NOT: allow(unused_assignments)

int printf(const char *, ...);

int default_then_override(int c) {
  int x = 7;
  if (c) x = 9;
  return x;
}

int store_then_break_then_read(int n) {
  int v = 0;
  int i;
  for (i = 0; i < n; i++) {
    if (i == 3) { v = i * 10; break; }
  }
  return v;
}

int loop_carry(int n) {
  int prev = 0, sum = 0, i;
  for (i = 0; i < n; i++) {
    sum += prev;
    prev = i;
  }
  return sum;
}

int cond_read_after_loop(int n) {
  int t = 0, i;
  for (i = 0; i < n; i++) {
    t = i;
    if (i & 1) continue;
    printf("t=%d\n", t);
  }
  return t;
}

int main(void) {
  printf("%d %d %d %d\n", default_then_override(1),
         store_then_break_then_read(5), loop_carry(4),
         cond_read_after_loop(4));
  return 0;
}

// The four functions still emit, each carrying its store: the predicate
// answers "live", it does not answer "no store here".
// CHECK: fn default_then_override
// CHECK: fn store_then_break_then_read
// CHECK: fn loop_carry
// CHECK:     prev = i;
// CHECK: fn cond_read_after_loop
