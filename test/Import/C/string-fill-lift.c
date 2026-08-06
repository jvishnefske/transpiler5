// RUN: emitrust-import-c %s | FileCheck %s

// FR-64 (the W4.5 heap memory-model change): a runtime-sized `char` buffer
// filled with a compile-time-constant ASCII byte over a canonical [0,len)
// counted loop, NUL-terminated, and consumed only as a C string lifts WHOLE to
// an idiomatic owned `String`. The `malloc`, the fill loop, and the NUL store
// are FUSED into a single `emitrust.string_repeat` binding; `puts` prints the
// `String` by `Display`; `free` is a no-op (the `String` drops at scope end).
// This is the ONLY safe lowering of the idiom: `String` has no `IndexMut`, so
// a literal per-byte write would need `unsafe`, which the project bans.

#include <stdlib.h>

int puts(const char *);

void alloc_string_a(unsigned int len) {
  char *a = malloc(len + 1);
  for (int i = 0; i < len; ++i)
    a[i] = 'a';
  a[len] = '\0';
  puts(a);
  free(a);
}

// CHECK-LABEL: func.func @alloc_string_a
// The whole idiom fuses into one `String::repeat` binding: the count is the
// loop's upper bound `len`, widened to i64.
// CHECK: %[[CNT:.*]] = emitrust.cast %{{.*}} : ui32 to i64
// CHECK: %[[S:.*]] = emitrust.string_repeat "a", %[[CNT]] : i64 -> <"String">
// CHECK: emitrust.assign %{{.*}} = %[[S]] : !emitrust.lvalue<!emitrust.opaque<"String">>
// The fill loop and NUL store are fused away: between the binding and the
// consumer NO per-byte subscript write and NO loop survive.
// CHECK-NOT: emitrust.subscript
// CHECK-NOT: scf.for
// CHECK-NOT: cf.br
// `puts` prints the `String` by Display — no i8-slice `%s` machinery.
// CHECK: emitrust.call_opaque "println!"(%{{.*}}) {{.*}} : (!emitrust.opaque<"String">) -> ()
// `free` leaves no trace — the `String` drops at scope end.
// CHECK-NOT: func.call @free
// CHECK: return
