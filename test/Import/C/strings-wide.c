// RUN: emitrust-import-c %s | FileCheck %s

// CTS-L3: a block-scope `wchar_t s[] = L"..."` initializer assigns the
// literal's code units (plus the terminating NUL) element by element into
// an i32 (`wchar_t`) array, exactly like the byte path of an ordinary
// literal — but without the ASCII limit, since a wide array never feeds
// the byte-string `%s`/`%c` printing helpers. Walking the array through a
// `wchar_t *` cursor is the ordinary CTS-P1 local region (the 00220
// shape).

#include <stddef.h>

// A file-scope wide array folds to a typed i32 element list through the
// constant-evaluator path; wide code units carry no ASCII limit.
wchar_t wg[] = L"h€";
// CHECK: emitrust.global @wg <[104 : i32, 8364 : i32, 0 : i32]> : !emitrust.array<3xi32>

// The euro sign is one code unit (0x20AC) in the wide encoding.
// CHECK-LABEL: func.func @c_main
// CHECK: %[[W:.*]] = emitrust.variable named "s" : !emitrust.lvalue<!emitrust.array<4xi32>>
// CHECK: emitrust.subscript %[[W]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>
// CHECK: arith.constant 104 : i32
// CHECK: arith.constant 8364 : i32
// CHECK: arith.constant 122 : i32
// CHECK: arith.constant 0 : i32
int main(void) {
  wchar_t s[] = L"h€z";
  wchar_t *p;
  int n = 0;
  for (p = s; *p; p++)
    n++;
  return n - 3;
}
