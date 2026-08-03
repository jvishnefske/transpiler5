// RUN: emitrust-import-c %s | FileCheck %s

// C99-43 slice 1b: the paired out-cursor T** parameter (Shape P, the
// strtol/endp family).
//
// Pins the invariant that a T** parameter with NO reads of `*p` and
// exactly ONE unconditional top-level write `*p = <expr>`, whose RHS
// roots in exactly one same-element slice-classified co-parameter,
// lowers to ONE `&mut i64` input (no slice — the callee never touches
// the content through it, and the co-parameter's slice already carries
// the region). The write assigns the RHS's cursor value (in the
// co-parameter's slice coordinates) straight through the reference: no
// entry cell, no return-site writeback. At the call, the caller passes
// the address of a staged i64 temp initialized to 0 and, after the
// call, stores co-argument-cursor-at-call + temp into the `&e`
// argument's cursor cell — the reslice coordinate correction
// (emitBorrowArgument reslices ordinary slice arguments AT the
// caller's cursor, so callee coordinate 0 is the co-argument's cursor,
// not the region start). The `&e` argument also JOINS `e` into the
// co-argument's region, which is what lets `q - t` below emit as a
// same-object cursor difference. The runtime observable is pinned
// differentially in test/EndToEnd/cursor-param-paired.c.

long parse(const char *s, const char **endp) {
  long i = 0;
  while (s[i] == ' ')
    i = i + 1;
  *endp = s + i;
  return i;
}
// One mut_ref<i64> input for `endp`, named after the C parameter (the
// co-parameter `s` keeps its ordinary slice lowering, mutable because
// only const u8 slices borrow shared, FR-55).
// CHECK-LABEL: func.func @parse
// CHECK-SAME: (%{{[^,)]+}}: !emitrust.mut_ref<!emitrust.slice<i8>>, %{{[^,)]+}}: !emitrust.mut_ref<i64>) -> i64
// CHECK-SAME: emitrust.param_names = ["s", "endp"]
// The write goes straight through the deref'd reference; no cell.
// CHECK: %[[OUT:.+]] = emitrust.deref %arg1 : (!emitrust.mut_ref<i64>) -> !emitrust.lvalue<i64>
// CHECK: emitrust.assign %[[OUT]] =

int main(void) {
  char t[4] = "  x";
  const char *q;
  long skipped = parse(t, &q);
  return (int)(skipped - (q - t));
}
// CHECK-LABEL: func.func @c_main
// The staged out-cursor temp starts at ZERO (the callee overwrites it
// unconditionally; the init only satisfies definite-assignment)...
// CHECK: %[[SL:.+]] = emitrust.slice_of mut %{{.+}} : (!emitrust.lvalue<!emitrust.array<4xi8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>
// CHECK-NEXT: %[[TMP:.+]] = emitrust.variable : !emitrust.lvalue<i64>
// CHECK-NEXT: %[[Z:.+]] = arith.constant 0 : i64
// CHECK-NEXT: emitrust.assign %[[TMP]] = %[[Z]]
// CHECK-NEXT: %[[REF:.+]] = emitrust.addr_of mut %[[TMP]] : (!emitrust.lvalue<i64>) -> !emitrust.mut_ref<i64>
// CHECK-NEXT: call @parse(%[[SL]], %[[REF]]) : (!emitrust.mut_ref<!emitrust.slice<i8>>, !emitrust.mut_ref<i64>) -> i64
// ...and after the call the written value (co-argument coordinates,
// here a direct whole-array decay, cursor 0) lands in q's cursor cell.
// CHECK-NEXT: %[[W:.+]] = emitrust.load %[[TMP]] : (!emitrust.lvalue<i64>) -> i64
// CHECK-NEXT: memref.store %[[W]]
