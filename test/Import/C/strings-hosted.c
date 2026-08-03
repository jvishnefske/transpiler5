// C99-48 / CTS-L1: hosted <string.h> subset over the C99-28 string model.
// Definition-less calls lower by name to one-per-module safe Rust helpers
// over i8 slices: the destination/source regions (char arrays, string
// literal backings, or pointers into either) are borrowed as
// `emitrust.slice_of` byte slices from their cursors, so every helper
// access is a bounds-checked slice index — no unsafe anywhere. strchr and
// strrchr return a found index (or -1 for C's NULL), consumed by the
// printf %s interception and by null-pointer comparisons.
// RUN: emitrust-import-c %s | FileCheck %s

#include <stdio.h>
#include <string.h>

int main(void) {
  char a[10];

  // strcpy of a literal into a char array: mutable destination slice at
  // cursor 0, shared borrow of the literal's read-only backing.
  strcpy(a, "hello");
  // CHECK-LABEL: func.func @c_main
  // CHECK: %[[A:.*]] = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.array<10xi8>>
  // CHECK: %[[LIT:.*]] = emitrust.variable const <[104 : i8, 101 : i8, 108 : i8, 108 : i8, 111 : i8, 0 : i8]>
  // CHECK: %[[DST:.*]] = emitrust.slice_of mut %[[A]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<10xi8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>
  // CHECK: %[[SRC:.*]] = emitrust.slice_of %[[LIT]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<6xi8>>, i64) -> !emitrust.ref<!emitrust.slice<i8>>
  // CHECK: emitrust.call_opaque "__emitrust_strcpy"(%[[DST]], %[[SRC]])

  // The copied-into array reads back through the %s machinery.
  printf("%s\n", a);
  // CHECK: emitrust.call_opaque "__emitrust_cstr"
  // CHECK: emitrust.call_opaque "print!"

  // strncpy carries its count as i64.
  strncpy(a, "gosh", 2);
  // CHECK: emitrust.call_opaque "__emitrust_strncpy"(%{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<i8>>, !emitrust.ref<!emitrust.slice<i8>>, i64) -> ()

  // strcat finds the destination NUL inside the helper.
  strcat(a, "!");
  // CHECK: emitrust.call_opaque "__emitrust_strcat"

  // Comparisons are value-position calls returning C's int.
  int cmp = strcmp(a, "apple") + strncmp(a, "go", 2);
  // CHECK: emitrust.call_opaque "__emitrust_strcmp"(%{{.*}}, %{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>, !emitrust.ref<!emitrust.slice<i8>>) -> i32
  // CHECK: emitrust.call_opaque "__emitrust_strncmp"

  // strlen now accepts a char array (not only literal-bound pointers);
  // the u64 result narrows per the call's declared size_t type.
  int len = (int)strlen(a);
  // CHECK: emitrust.call_opaque "__emitrust_strlen"(%{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>) -> i64

  // strchr feeding %s: the helper's relative index offsets the argument's
  // cursor and the region is re-sliced there for __emitrust_cstr.
  printf("%s\n", strchr(a, 'o'));
  // CHECK: %[[IDX:.*]] = emitrust.call_opaque "__emitrust_strchr"(%{{.*}}, %{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>, i32) -> i64
  // CHECK: %[[AT:.*]] = arith.addi %{{.*}}, %[[IDX]] : i64
  // CHECK: emitrust.slice_of %[[A]][%[[AT]]]
  // CHECK: emitrust.call_opaque "__emitrust_cstr"

  // strrchr against NULL folds to an index test against -1.
  printf("%d\n", strrchr(a, 'x') == NULL);
  // CHECK: %[[RIDX:.*]] = emitrust.call_opaque "__emitrust_strrchr"
  // CHECK: %[[NF:.*]] = arith.constant -1 : i64
  // CHECK: arith.cmpi eq, %[[RIDX]], %[[NF]] : i64

  // %s of a pointer to an array element slices from that element.
  printf("%s\n", &a[1]);
  // CHECK: emitrust.slice_of %[[A]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<10xi8>>, i64) -> !emitrust.ref<!emitrust.slice<i8>>
  // CHECK: emitrust.call_opaque "__emitrust_cstr"

  // memset through a void* parameter strips the implicit bitcast and
  // borrows the destination mutably from &a[1]'s cursor.
  memset(&a[1], 'r', 4);
  // CHECK: emitrust.call_opaque "__emitrust_memset"(%{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<i8>>, i32, i64) -> ()

  // memcpy with both arguments in the same array would alias a mutable
  // borrow, so the array is borrowed mutably once and the helper receives
  // both cursors (copy_within).
  memcpy(&a[2], a, 2);
  // CHECK: emitrust.call_opaque "__emitrust_memcpy_within"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<i8>>, i64, i64, i64) -> ()

  // memcpy between distinct regions borrows two slices.
  char b[10];
  memcpy(b, a, 4);
  // CHECK: emitrust.call_opaque "__emitrust_memcpy"(%{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<i8>>, !emitrust.ref<!emitrust.slice<i8>>, i64) -> ()

  // memcmp compares byte runs with an i64 count.
  int mem = memcmp(a, "grgr", 4);
  // CHECK: emitrust.call_opaque "__emitrust_memcmp"(%{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>, !emitrust.ref<!emitrust.slice<i8>>, i64) -> i32

  return cmp + len + mem;
}

// Each requested helper is emitted exactly once, as safe Rust.
// CHECK: emitrust.verbatim "fn __emitrust_strcpy(dst: &mut [i8], src: &[i8])
// CHECK: emitrust.verbatim "fn __emitrust_strncpy(dst: &mut [i8], src: &[i8], n: i64)
// CHECK: emitrust.verbatim "fn __emitrust_strcat(dst: &mut [i8], src: &[i8])
// CHECK: emitrust.verbatim "fn __emitrust_strcmp(a: &[i8], b: &[i8]) -> i32
// CHECK: emitrust.verbatim "fn __emitrust_strncmp(a: &[i8], b: &[i8], n: i64) -> i32
// CHECK: emitrust.verbatim "fn __emitrust_strchr(s: &[i8], c: i32) -> i64
// CHECK: emitrust.verbatim "fn __emitrust_strrchr(s: &[i8], c: i32) -> i64
// CHECK: emitrust.verbatim "fn __emitrust_memset(s: &mut [i8], c: i32, n: i64)
// CHECK: emitrust.verbatim "fn __emitrust_memcpy(dst: &mut [i8], src: &[i8], n: i64)
// CHECK: emitrust.verbatim "fn __emitrust_memcpy_within(s: &mut [i8], dst: i64, src: i64, n: i64)
// CHECK: emitrust.verbatim "fn __emitrust_memcmp(a: &[i8], b: &[i8], n: i64) -> i32
// CHECK-NOT: unsafe
