// FR-250: the Rust side of the subset is DEFINED by rs_parse: anything it
// does not model is a located error, never a guess. Here: a typed literal
// mismatch (no inference in the subset) and an unsigned `+` (debug-build
// panic). Well-formedness errors are ALL reported, like C++ residuals; only
// a SYNTAX error stops the Rust parse at the first one.
// RUN: not %emitrust-bx %s 2>&1 | FileCheck %s
fn main() {
    // CHECK: rust-side.rs:[[@LINE+1]]:20: error: expression has type i32, expected u64
    let big: u64 = 5;
    // CHECK-NEXT: rust-side.rs:[[@LINE+1]]:22: error: unsupported: unsigned `+` (panics on overflow in debug builds); the subset spells it `.wrapping_add(..)`
    let s: u64 = big + 1u64;
}
