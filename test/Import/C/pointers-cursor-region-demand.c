// RUN: emitrust-import-c %s | FileCheck %s

// FR-153: the `T **` cursor parameter's REGION base borrows mutably IFF
// the callee's own body demands it.
//
// A cursor parameter lowers to TWO inputs — a region slice plus an in-out
// i64 cursor. The region slice used to be unconditionally SHARED, which
// broke every body that forwards `*p` on to a parameter mapping to a
// mutable borrow (rustc E0596; the systemd `notify_on_cleanup` shape).
// The fix is deliberately NOT "make every cursor region mutable": that
// hands out `&mut` to regions nothing writes and prices in two fresh
// hazards (two cursors over one region become E0499, and a literal-backed
// region becomes E0596 at the caller).
//
// So this file pins the SPLIT, and it is the test that stops a later
// refactor collapsing the demand analysis back into "always mutable":
// three cursor parameters that differ ONLY in what their bodies do with
// `*p` must get three answers, and the caller's `slice_of` mutability
// must follow the callee's slot rather than being chosen locally.
//
// The only admitted demand is forwarding `*p` to a parameter that maps to
// a mutable borrow. Writes THROUGH the cursor are already a located
// rejection in planning, so they need no handling here. Deliberately NOT
// admitted: a demand reached through a local copy (`q = *p; g(q);`).
// Missing a demand is the safe direction — it leaves today's behaviour
// and today's E0596, never a miscompile.

// (1) No forwarding at all: reads under `*p` and the `*s = *s + 1`
// advancement. Nothing demands a mutable region, so the base stays the
// historical SHARED slice.
int take_byte(const char **s) {
  char c = **s;
  *s = *s + 1;
  return c;
}
// CHECK-LABEL: func.func @take_byte
// CHECK-SAME: (%{{[^,)]+}}: !emitrust.ref<!emitrust.slice<i8>>, %{{[^,)]+}}: !emitrust.mut_ref<i64>) -> i32

// (2) Forwarding `*p` to a callee whose parameter maps to a SHARED slice
// (`const unsigned char *` is the CTS-BR shared byte-slice rule). A call
// is not itself a demand — the callee's slot is what decides — so the
// base stays shared and the forwarded view is a shared `slice_of`.
unsigned sum_u8(const unsigned char *s) {
  return (unsigned)s[0] + (unsigned)s[1];
}
// CHECK-LABEL: func.func @sum_u8
// CHECK-SAME: (%{{[^,)]+}}: !emitrust.ref<!emitrust.slice<ui8>>)

unsigned walk_u8(const unsigned char **p) { return sum_u8(*p); }
// CHECK-LABEL: func.func @walk_u8
// CHECK-SAME: (%{{[^,)]+}}: !emitrust.ref<!emitrust.slice<ui8>>, %{{[^,)]+}}: !emitrust.mut_ref<i64>) -> ui32
// CHECK: emitrust.slice_of %{{[^ ]+}}[%{{[^]]+}}] : (!emitrust.lvalue<!emitrust.slice<ui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>

// (3) Forwarding `*p` to a callee whose parameter maps to a MUTABLE slice
// (`const char *` still maps mutable). THIS is the admitted demand, and
// the region base becomes `!emitrust.mut_ref`.
int sd_notify(int unset, const char *state) { return (int)state[0] + unset; }
// CHECK-LABEL: func.func @sd_notify
// CHECK-SAME: (%{{[^,)]+}}: i32, %{{[^,)]+}}: !emitrust.mut_ref<!emitrust.slice<i8>>) -> i32

int notify_on_cleanup(const char **p, int unset) {
  if (*p)
    return sd_notify(unset, *p);
  return -1;
}
// CHECK-LABEL: func.func @notify_on_cleanup
// CHECK-SAME: (%{{[^,)]+}}: !emitrust.mut_ref<!emitrust.slice<i8>>, %{{[^,)]+}}: !emitrust.mut_ref<i64>, %{{[^,)]+}}: i32) -> i32
// CHECK: emitrust.slice_of mut %{{[^ ]+}}[%{{[^]]+}}] : (!emitrust.lvalue<!emitrust.slice<i8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>

// Caller side, shared callee: TWO cursor arguments walking the SAME
// region are fine when the callee's base slot is shared, so the
// same-region aliasing rejection must NOT fire here. Both region
// arguments are shared `slice_of`s over the one array place.
unsigned two_shared(void) {
  unsigned char buf[5] = {1, 2, 3, 4, 0};
  const unsigned char *a = buf;
  const unsigned char *b = buf + 2;
  return walk_u8(&a) + walk_u8(&b);
}
// CHECK-LABEL: func.func @two_shared
// CHECK: %[[SH1:.+]] = emitrust.slice_of %{{[^ ]+}}[%{{[^]]+}}] : (!emitrust.lvalue<!emitrust.array<5xui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: call @walk_u8(%[[SH1]]
// CHECK: %[[SH2:.+]] = emitrust.slice_of %{{[^ ]+}}[%{{[^]]+}}] : (!emitrust.lvalue<!emitrust.array<5xui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: call @walk_u8(%[[SH2]]

// Caller side, mutable callee: the argument's `is_mut` follows the
// CALLEE's slot type, so the same syntactic call site now hands out a
// mutable whole-region view.
int call_notify(void) {
  char buf[3] = {'a', 'b', 0};
  const char *c = buf;
  return notify_on_cleanup(&c, 1);
}
// CHECK-LABEL: func.func @call_notify
// CHECK: %[[MUT:.+]] = emitrust.slice_of mut %{{[^ ]+}}[%{{[^]]+}}] : (!emitrust.lvalue<!emitrust.array<3xi8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>
// CHECK: call @notify_on_cleanup(%[[MUT]]
