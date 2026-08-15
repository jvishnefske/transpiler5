// RUN: emitrust-import-c %s | FileCheck %s

// FR-93, WINDOW-backed variant: a pointer local bound to a member
// array of a BYTE-REGION record (aes.c:504's `uint8_t *Iv = ctx->Iv;`,
// the last tiny-AES-c blocker). A byte-region record's members are
// windows of ONE flat u8 region, not places (FR-91), so the local's
// backing is the region ROOT itself — the struct-pointer parameter's
// deref'd byte-slice place, or a local byte-region aggregate's flat
// byte array — and the i64 cursor is an ABSOLUTE byte offset that
// starts at the field's layout offset (`resolveByteRegionRef` +
// `byteRegionOffset`, the FR-91 window convention). Deref, subscript,
// arithmetic, and argument passing then compose through the EXISTING
// slice-region machinery unchanged, because the base behaves exactly
// like a slice parameter base. This pins the init and assign forms
// over both window roots so a regression cannot shift the cursor
// origin off the field offset (which would silently walk the wrong
// region bytes — the reason the EndToEnd byte-diff twin exists).

typedef unsigned char u8;
struct B { u8 rk[8]; u8 iv[4]; };

static u8 fold(const u8 *xs, int n) {
  u8 acc = 0;
  int i;
  for (i = 0; i < n; i++)
    acc = (u8)(acc ^ xs[i]);
  return acc;
}

// Arrow root: `p = b->iv` binds the parameter's byte region with the
// cursor at layout offset 8 plus the parameter's own runtime cursor;
// the walk and the reads subscript the region place absolutely.
static u8 win(struct B *b, int k) {
  const u8 *p = b->iv;
  u8 a;
  p += k;
  a = (u8)(p[1] ^ *p);
  p = b->iv;
  return (u8)(a + fold(p, 4));
}
// CHECK-LABEL: func.func @win
//   the parameter's region place: one entry-block deref of &mut [u8].
// CHECK: %[[REG:.*]] = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.slice<ui8>>) -> !emitrust.lvalue<!emitrust.slice<ui8>>
//   the init binding: cursor = field byte offset 8 (+ the parameter's
//   runtime cursor), stored into the local's cursor cell.
// CHECK: %[[OFF:.*]] = arith.constant 8 : i64
// CHECK: arith.addi %[[OFF]]
// CHECK: memref.store
//   deref/subscript read the region place at the absolute cursor.
// CHECK: emitrust.subscript %[[REG]]
//   the argument use reslices the region at the current cursor.
// CHECK: emitrust.slice_of %[[REG]][%{{.*}}] : (!emitrust.lvalue<!emitrust.slice<ui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: call @fold

// Dot root: a local byte-region aggregate is its own flat byte array;
// the member decay binds it with the absolute byte cursor.
static u8 dotwin(int k) {
  struct B s;
  const u8 *q;
  s.iv[0] = (u8)k;
  s.iv[1] = (u8)(k + 1);
  q = s.iv;
  q += 1;
  return *q;
}
// CHECK-LABEL: func.func @dotwin
// CHECK: %[[LOC:.*]] = emitrust.variable named "s" : !emitrust.lvalue<!emitrust.array<12xui8>>
//   init binding at byte offset 8.
// CHECK: %[[OFF2:.*]] = arith.constant 8 : i64
// CHECK: memref.store
// CHECK: emitrust.subscript %[[LOC]]

int main(void) {
  struct B b;
  int i;
  for (i = 0; i < 8; i++)
    b.rk[i] = (u8)i;
  for (i = 0; i < 4; i++)
    b.iv[i] = (u8)(i + 8);
  return win(&b, 1) + dotwin(3);
}
