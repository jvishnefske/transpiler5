// RUN: emitrust-import-c %s | FileCheck %s

// FR-65 (the W4.5 heap memory-model change, Vec arm of the {array, Vec, span,
// Option} representation match): a runtime-sized heap buffer of a non-char
// scalar element type (`int *a = malloc(n * sizeof(int))`, runtime `n`) used
// ONLY through `a[i]` indexing and `free` lifts WHOLE to an owned `Vec<T>`.
// The allocation becomes a single `emitrust.vec_fill` binding (`vec![0i32; n as
// usize]`); every `a[i]` read AND write becomes an `emitrust.subscript` over
// the `Vec` place (`Vec<T>` has `IndexMut`, so arbitrary fills are safe — no
// constant-fill restriction, unlike the FR-64 `String` arm); `free` is a no-op
// (the `Vec` drops at scope end).

#include <stdlib.h>
int printf(const char *, ...);

void fill_squares(unsigned int n) {
  int *a = malloc(n * sizeof(int));
  for (unsigned int i = 0; i < n; ++i)
    a[i] = (int)(i * i);
  int s = 0;
  for (unsigned int i = 0; i < n; ++i)
    s += a[i];
  printf("%d\n", s);
  free(a);
}

// CHECK-LABEL: func.func @fill_squares
// The runtime count `n` (widened to i64) fills a fresh `Vec<i32>`; the suffixed
// zero literal is the sound refinement of malloc's indeterminate bytes.
// CHECK: %[[CNT:.*]] = emitrust.cast %{{.*}} : ui64 to i64
// CHECK: %[[V:.*]] = emitrust.vec_fill "0i32", %[[CNT]] : i64 -> <"Vec<i32>">
// CHECK: emitrust.assign %[[A:.*]] = %[[V]] : !emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>
// The fill write `a[i] = i*i` is a real `Vec` index write (kept, not elided).
// CHECK: %[[W:.*]] = emitrust.subscript %[[A]][%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>, ui32) -> !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[W]] = %{{.*}} : !emitrust.lvalue<i32>
// The read `s += a[i]` subscripts the same `Vec` place and loads it.
// CHECK: %[[R:.*]] = emitrust.subscript %[[A]][%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>, ui32) -> !emitrust.lvalue<i32>
// CHECK: emitrust.load %[[R]] : (!emitrust.lvalue<i32>) -> i32
// `free` leaves no trace — the `Vec` drops at scope end.
// CHECK-NOT: func.call @free
// CHECK: return
