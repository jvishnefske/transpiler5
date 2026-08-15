// RUN: emitrust-import-c %s | FileCheck %s

// FR-71: a `void *` PARAMETER whose every body use converts to ONE
// consistent byte pointee (uint8_t*/unsigned char*/char*) imports as a
// byte-slice cursor parameter under the existing slice-param convention
// (ParamKind::Slice): the signature input is `!emitrust.mut_ref<
// !emitrust.slice<ui8>>` (`!emitrust.ref<...>` for `const void *`), the
// body's byte-pointer local decomposes into (deref'd backing, i64 cursor
// alloca), and element traffic is `emitrust.subscript` — byte-for-byte
// the shape the same body gets when the parameter is spelled `uint8_t *`
// directly. This pins the tinycrypt `_set` (memset-alike) and `_compare`
// shapes, the top-ranked void*-parameter diagnostic of the 2026-08-14
// Track 5 re-measurement, plus the char-pointee (i8) and explicit-cast
// forms and the call-site `emitrust.slice_of [mut]` lowering. Everything
// outside this subset keeps the verbatim located rejection (see
// void-param-invalid.c).

typedef unsigned char uint8_t;
typedef unsigned long size_t;

void set_bytes(void *to, uint8_t val, unsigned len) {
  uint8_t *t = to;
  while (len--) {
    *t++ = val;
  }
}

// The non-const `void *` maps like a mutable byte slice; the body is the
// (backing, cursor) decomposition of the byte-pointer local.
// CHECK-LABEL: func.func @set_bytes
// CHECK-SAME: (%arg0: !emitrust.mut_ref<!emitrust.slice<ui8>>, %arg1: ui8, %arg2: ui32)
// CHECK: memref.alloca() : memref<i64>
// CHECK: emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.slice<ui8>>) -> !emitrust.lvalue<!emitrust.slice<ui8>>
// CHECK: emitrust.subscript
// CHECK-SAME: (!emitrust.lvalue<!emitrust.slice<ui8>>, i64) -> !emitrust.lvalue<ui8>

int compare_bytes(const void *a, const void *b, size_t size) {
  const uint8_t *tempa = a;
  const uint8_t *tempb = b;
  uint8_t result = 0;
  for (size_t i = 0; i < size; i++)
    result |= tempa[i] ^ tempb[i];
  return result;
}

// The const form borrows shared: `const void *` maps like a walked
// `const uint8_t *` (`&[u8]`), and reads subscript the deref'd backing.
// CHECK-LABEL: func.func @compare_bytes
// CHECK-SAME: (%arg0: !emitrust.ref<!emitrust.slice<ui8>>, %arg1: !emitrust.ref<!emitrust.slice<ui8>>, %arg2: ui64) -> i32
// CHECK: emitrust.deref %arg0 : (!emitrust.ref<!emitrust.slice<ui8>>) -> !emitrust.lvalue<!emitrust.slice<ui8>>
// CHECK: emitrust.deref %arg1 : (!emitrust.ref<!emitrust.slice<ui8>>) -> !emitrust.lvalue<!emitrust.slice<ui8>>
// CHECK: emitrust.subscript
// CHECK-SAME: (!emitrust.lvalue<!emitrust.slice<ui8>>, i64) -> !emitrust.lvalue<ui8>

int first_char(void *p) {
  // An EXPLICIT cast to the byte pointee qualifies exactly like the
  // implicit conversion, and a consistent `char *` pointee picks the i8
  // element.
  char *c = (char *)p;
  return c[0];
}

// CHECK-LABEL: func.func @first_char
// CHECK-SAME: (%arg0: !emitrust.mut_ref<!emitrust.slice<i8>>) -> i32

int main(void) {
  uint8_t buf[8];
  uint8_t ref[8];
  char text[4];
  set_bytes(buf, 7, 8u);
  set_bytes(ref, 7, 8u);
  text[0] = 'a';
  return compare_bytes(buf, ref, 8) + first_char(text);
}

// Call sites lower through the existing slice-argument convention: a
// byte-array argument reslices from cursor 0, mutably for the non-const
// parameter and shared for the const one.
// CHECK-LABEL: func.func @c_main
// CHECK: emitrust.slice_of mut %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<8xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK: call @set_bytes(%{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, ui8, ui32) -> ()
// CHECK: emitrust.slice_of %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<8xui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: call @compare_bytes(%{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.ref<!emitrust.slice<ui8>>, !emitrust.ref<!emitrust.slice<ui8>>, ui64) -> i32
// CHECK: emitrust.slice_of mut %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>
// CHECK: call @first_char(%{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<i8>>) -> i32
