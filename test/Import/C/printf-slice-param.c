// RUN: emitrust-import-c %s | FileCheck %s --implicit-check-not='"__emitrust_cstr"'

// CTS-L2 (design.md): printf %s of a `char *` function parameter. A
// slice-classified pointer parameter (FR-28, `mut_ref<slice<i8>>`) feeds
// %s as an `emitrust.slice_of` of its deref'd slice base place at the
// parameter's current cursor. FR-191: that slice is written RAW through
// `__emitrust_cstr_out` (stop-at-first-NUL, like C) rather than through the
// Latin-1 `__emitrust_cstr` Display funnel, which re-encoded every byte
// >= 0x80 as two UTF-8 bytes; the pending format text flushes as its own
// `print!` so the raw write lands in program order. A string-literal argument
// to a slice
// parameter materializes a fresh mutable backing byte array (bytes plus
// the terminating NUL) at the call site and passes a whole-array slice;
// per-call copies are unobservable in defined C programs because writing
// through a pointer to a string literal is undefined behavior.

int printf(const char *fmt, ...);

// The %s use classifies `s` as a slice parameter; the second printf
// prints from the walked cursor (C's `s + 1` suffix semantics).
static void show(const char *s) {
  printf("[%s]\n", s);
  s++;
  printf("[%s]\n", s);
}
// CHECK-LABEL: func.func @show
// CHECK-SAME: (%[[S:.*]]: !emitrust.mut_ref<!emitrust.slice<i8>>)
// CHECK: %[[CUR:.*]] = memref.alloca() : memref<i64>
// CHECK: %[[BASE:.*]] = emitrust.deref %[[S]] : (!emitrust.mut_ref<!emitrust.slice<i8>>) -> !emitrust.lvalue<!emitrust.slice<i8>>
//   First %s: slice the base at the (zero) cursor, print through the helper.
// CHECK: %[[C0:.*]] = memref.load %[[CUR]][] : memref<i64>
// CHECK: %[[SL0:.*]] = emitrust.slice_of %[[BASE]][%[[C0]]] : (!emitrust.lvalue<!emitrust.slice<i8>>, i64) -> !emitrust.ref<!emitrust.slice<i8>>
// CHECK: emitrust.call_opaque "print!"() {args = ["["]}
// CHECK: emitrust.call_opaque "__emitrust_cstr_out"(%[[SL0]]) : (!emitrust.ref<!emitrust.slice<i8>>) -> ()
// CHECK: emitrust.call_opaque "println!"() {args = ["]"]}
//   s++ advances the cursor cell; the second %s slices from the new cursor.
// CHECK: arith.addi
// CHECK: %[[C1:.*]] = memref.load %[[CUR]][] : memref<i64>
// CHECK: %[[SL1:.*]] = emitrust.slice_of %[[BASE]][%[[C1]]] : (!emitrust.lvalue<!emitrust.slice<i8>>, i64) -> !emitrust.ref<!emitrust.slice<i8>>
// CHECK: emitrust.call_opaque "print!"() {args = ["["]}
// CHECK: emitrust.call_opaque "__emitrust_cstr_out"(%[[SL1]])
// CHECK: emitrust.call_opaque "println!"() {args = ["]"]}

int main(void) {
  char buf[4] = "ab";
  show("xyz");
  show(buf);
  return 0;
}
// CHECK-LABEL: func.func @c_main
//   The literal argument becomes a fresh mutable (non-const) backing with
//   the literal's bytes plus the NUL, passed as a whole-array &mut slice.
// CHECK: %[[LIT:.*]] = emitrust.variable <[120 : i8, 121 : i8, 122 : i8, 0 : i8]> : !emitrust.lvalue<!emitrust.array<4xi8>>
// CHECK: %[[LSL:.*]] = emitrust.slice_of mut %[[LIT]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>
// CHECK: call @show(%[[LSL]]) : (!emitrust.mut_ref<!emitrust.slice<i8>>) -> ()
//   The char-array argument reslices the array base as before.
// CHECK: %[[ASL:.*]] = emitrust.slice_of mut %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>
// CHECK: call @show(%[[ASL]])

// The stop-at-first-NUL raw-bytes helper is emitted once at module level,
// and the Display funnel it replaced is no longer requested at all (an
// unused helper would be an `unused` deny in the emitted crate).
// CHECK: emitrust.verbatim "fn __emitrust_cstr_out(s: &[i8]) {
