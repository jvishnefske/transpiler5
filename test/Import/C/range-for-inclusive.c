// FR-61f widening: the range-for matcher's clause 2 accepts the ascending
// INCLUSIVE bound `i <= HI` and marks the emitted `emitrust.for` with the
// `inclusive` unit attribute (rendered `LO..=HI`). C's only divergence from
// Rust's inclusive range is the `HI == INT_MAX` overflow of the final `i++`,
// which is C UB — Rust's `..=` terminating cleanly there is a legal
// refinement. Descending (`i > 0; i--`) stays OUT of the matcher and falls
// back to the CFG `while` lowering, pinning the frontier.
// RUN: emitrust-import-c %s | FileCheck %s

// CHECK-LABEL: func.func @inclusive_sum
// CHECK: emitrust.for
// CHECK: {inclusive}
int inclusive_sum(int n) {
  int s = 0;
  for (int i = 1; i <= n; i++)
    s = s + i;
  return s;
}

// CHECK-LABEL: func.func @countdown
// CHECK-NOT: emitrust.for
// CHECK: cf.br
int countdown(int n) {
  int s = 0;
  for (int i = n; i > 0; i--)
    s = s + i;
  return s;
}
