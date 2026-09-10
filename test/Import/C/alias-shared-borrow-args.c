// RUN: split-file %s %t
// RUN: emitrust-cc --emit=rust %t/cursors.c -o - | FileCheck %s --check-prefix=CURSORS
// RUN: emitrust-cc --emit=rust %t/byte-region-field.c -o - | FileCheck %s --check-prefix=FIELD
// RUN: emitrust-cc --emit=rust %t/heap.c -o - | FileCheck %s --check-prefix=HEAP

// FR-222: `emitCall`'s aliasing collision key now carries the borrow's
// MUTABILITY, so two SHARED borrows of one object no longer collide. That
// is not a relaxation of a safety rule — it is the rule the C++ arm has
// enforced since FR-203 (`heldRoot == argRoot && (argIsMut || heldIsMut)`,
// ImportCExpressions.cpp), whose own comment already asserted that
// `emitCall` applied it. It did not, and the disagreement was observable:
// `f(const unsigned char *a, const unsigned char *b)` called as
// `f(p, p + 4)` was refused with "aliasing mutable pointer arguments",
// a word that is factually false when NEITHER borrow is mutable, and the
// refused program is provably correct — the emitted crate builds clean and
// runs byte-identical to the clang native (pinned in
// test/EndToEnd/alias-shared-borrow-args.c, which is the actual oracle;
// these are the byte-identity pins for the emitted shape).
//
// What this file pins, shape by shape: a `const unsigned char *` parameter
// maps to `&[u8]`, so both borrows are shared and BOTH reslices of one
// region may live at once — at IDENTICAL cursors, at disjoint constant
// cursors, and at RUNTIME cursors nothing proves disjoint. Overlap is
// irrelevant to soundness here, which is exactly why a disjointness test
// is the wrong rule and mutability is the right one.
//
// The rejected side — a mutable borrow on EITHER side of the pair — is
// pinned next door in alias-shared-borrow-args-invalid.c and has to be
// read with this file: without it, admitting the shared pair would turn
// `f(p, p)` over `unsigned char *` into a rustc E0499 crate.

// Three shared-shared pairs over one PARAMETER-rooted region. A parameter
// root is deliberate: the Phase-4 owner lift is gated to caller-LOCAL
// regions by `planOwners`, so it cannot quietly promote these pointers
// into i64 indices and make the aliasing key moot.
//--- cursors.c
typedef unsigned char u8;
static int f(const u8 *a, const u8 *b) { return a[0] + b[0]; }
int run_same(u8 *p) { return f(p, p); }
int run_cursor(u8 *p) { return f(p, p + 4); }
int run_runtime(u8 *p, int i, int j) { return f(p + i, p + j); }
// CURSORS: fn tu0_f(a: &[u8], b: &[u8]) -> i32 {
// CURSORS-LABEL: pub fn run_same(p: &mut [u8]) -> i32 {
// CURSORS:         let v1: &[u8] = &(*p)[0i64 as usize..];
// CURSORS-NEXT:    let v2: &[u8] = &(*p)[0i64 as usize..];
// CURSORS-NEXT:    tu0_f(v1, v2)
// CURSORS-LABEL: pub fn run_cursor(p: &mut [u8]) -> i32 {
// CURSORS:         let v2: &[u8] = &(*p)[0i64 as usize..];
// CURSORS-NEXT:    let v3: &[u8] = &(*p)[4i64 as usize..];
// CURSORS-NEXT:    tu0_f(v2, v3)
// CURSORS-LABEL: pub fn run_runtime(p: &mut [u8], i: i32, j: i32) -> i32 {
// CURSORS:         let v1: &[u8] = &(*p)[i as i64 as usize..];
// CURSORS-NEXT:    let v3: &[u8] = &(*p)[j as i64 as usize..];
// CURSORS-NEXT:    tu0_f(v1, v3)

// An object and its OWN field, both borrowed SHARED. The all-u8 struct is
// one flat byte region, so the whole-object window and the field window
// overlap by prefix — and that is fine, because `&r[..]` beside `&r[4..]`
// is legal Rust. The MUTABLE spelling of this same shape stays a located
// rejection (obj-field.c next door): the prefix-overlap key is unchanged,
// only its mutability guard is new.
//--- byte-region-field.c
typedef unsigned char u8;
typedef struct { u8 x[4]; u8 y[4]; } T;
static int f(const T *t, const u8 *b) { return t->x[0] + b[0]; }
int run(T *t) { return f(t, t->y); }
// FIELD: fn tu0_f(t: &[u8], b: &[u8]) -> i32 {
// FIELD-LABEL: pub fn run(t: &mut [u8]) -> i32 {
// FIELD:         let v2: &[u8] = &(*t)[0i64 as usize..];
// FIELD-NEXT:    let v3: &[u8] = &(*t)[4i64 as usize..];
// FIELD-NEXT:    tu0_f(v2, v3)

// The heap arm of the same key (FR-147's backing-keyed identity) got the
// same guard, so the two halves of one rule cannot disagree with each
// other the way `emitCall` and `emitCXXMemberCall` did. Two SHARED
// open-ended windows on one allocation are `&v[i..]` beside `&v[j..]`.
// This arm is DEAD in the TRACTOR corpus (0 of 83 aliasing events name an
// allocation); it is fixed for consistency with its sibling, not yield.
//--- heap.c
#include <stdlib.h>
typedef unsigned char u8;
static int f(const u8 *a, const u8 *b) { return a[0] + b[0]; }
int run(void) {
  u8 *h = (u8 *)malloc(64);
  h[0] = 3;
  return f(h, h + 4);
}
// HEAP: fn tu0_f(a: &[u8], b: &[u8]) -> i32 {
// HEAP-LABEL: pub fn run() -> i32 {
// HEAP:         let v5: &[u8] = &v3[0i64 as usize..];
// HEAP-NEXT:    let v6: &[u8] = &v3[4i64 as usize..];
// HEAP-NEXT:    tu0_f(v5, v6)
