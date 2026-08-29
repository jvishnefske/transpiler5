// FR-142 (defect): the FR entry's reproducer, verbatim, pinned in the FAST
// tier. A deferred `long off` written once per `if` arm and then used only as
// the START INDEX of a mutable slice borrow (`sink(s + off)` -> `&mut
// (*s)[off as usize..]`) was emitted `let mut v7: i64;`. Emitted crates deny
// `unused_mut`, so the transpiler exited 0 and handed back a crate that does
// not compile:
//   error: variable does not need to be mutable  --> let mut v7: i64;
//
// `emitrust.slice_of` has two operands, `$base` and `$index`, and the
// post-init-mutation scan in `computeDeferredInits` scored `getIsMut()`
// without asking which one the binding was, so a pure READ standing in as the
// index counted as a mutable borrow of the binding itself.
//
// This is the cheapest standing guard for the shape: it needs no cargo, so it
// runs in the inner-loop tier, while test/EndToEnd/deferred-mut-slice-index.c
// carries the runtime byte-diff and the actual `cargo build`, and
// test/Target/Rust/deferred-mut-slice-index.mlir carries the base-vs-index
// invariance guard that keeps the fix from over-relaxing.
//
// With the spurious `mut` cleared, FR-61b's if-expression lift becomes
// eligible and folds the whole shape into `let v7: i64 = if avail < len { .. }
// else { 0i64 };`. That is the intended, corpus-visible rendering and it is
// pinned here line by line -- an `off` that reappeared as a statement-form
// `let v7: i64;` would still be correct Rust, so only the exact spelling pins
// which of the two the emitter chose.
//
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s

extern long grow(unsigned long n);
void sink(char *p);

// CHECK-LABEL: pub fn f<E: Externals>(s: &mut [i8], v0: u64, v1: u64) -> i64 {
// CHECK-NEXT:    let avail: u64 = v0;
// CHECK-NEXT:    let len: u64 = v1;
// CHECK-NEXT:    let [[OFF:v[0-9]+]]: i64 = if avail < len {
// CHECK-NEXT:      let [[G:v[0-9]+]]: i64 = E::grow(len.wrapping_sub(avail));
// CHECK-NEXT:      [[G]]
// CHECK-NEXT:    } else {
// CHECK-NEXT:      0i64
// CHECK-NEXT:    };
// CHECK-NEXT:    let [[SL:v[0-9]+]]: &mut [i8] = &mut (*s)[
// CHECK-SAME:    [[OFF]] as usize..];
// CHECK-NEXT:    E::sink([[SL]]);
// CHECK-NEXT:    [[OFF]]
// CHECK-NEXT:  }
long f(char *s, unsigned long avail, unsigned long len) {
    long off;
    if (avail < len) {
        off = grow(len - avail);
    } else {
        off = 0;
    }
    sink(s + off);
    return off;
}
