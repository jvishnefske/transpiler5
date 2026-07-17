// RUN: emitrust-import-c %s | FileCheck %s

// CTS-P1: a `char *` bound to a string literal is a cursor into a
// read-only region. The literal's bytes (plus the terminating NUL, which
// is what ends strlen-style walks) back an immutable `const`-marked
// emitrust.variable byte array; the pointer is the usual i64 cursor in a
// rank-0 memref cell, dereference and subscript read bytes out of the
// backing, and pointer arithmetic is cursor arithmetic. Writes through the
// region are rejected (see pointers-local-invalid.c).

// The literal (adjacent pieces concatenate in C) becomes a 6-byte backing
// including the NUL; walking with ++ terminates on the NUL byte.
int walk(void) {
  char *p = "hel" "lo";
  int n = 0;
  while (*p != 0) {
    n = n + 1;
    p++;
  }
  return n;
}
// CHECK-LABEL: func.func @walk
// CHECK: %[[CELL:.*]] = memref.alloca() : memref<i64>
// CHECK: %[[LIT:.*]] = emitrust.variable const <[104 : i8, 101 : i8, 108 : i8, 108 : i8, 111 : i8, 0 : i8]> : !emitrust.lvalue<!emitrust.array<6xi8>>
//   p = "hello" stores cursor 0.
// CHECK: %[[ZERO:.*]] = arith.constant 0 : i64
// CHECK: memref.store %[[ZERO]], %[[CELL]][] : memref<i64>
//   *p reads a byte of the backing at the loaded cursor.
// CHECK: %[[CUR:.*]] = memref.load %[[CELL]][] : memref<i64>
// CHECK: %[[ELEM:.*]] = emitrust.subscript %[[LIT]][%[[CUR]]] : (!emitrust.lvalue<!emitrust.array<6xi8>>, i64) -> !emitrust.lvalue<i8>
// CHECK: emitrust.load %[[ELEM]] : (!emitrust.lvalue<i8>) -> i8
//   p++ is cursor arithmetic.
// CHECK: %[[C:.*]] = memref.load %[[CELL]][]
// CHECK: %[[N:.*]] = arith.addi %[[C]], %{{.*}} : i64
// CHECK: memref.store %[[N]], %[[CELL]][] : memref<i64>

// Two pointers bound into the same literal share one backing; subscript
// and index arithmetic in both directions read relative to the cursor.
int shared(void) {
  char *p = "abc";
  char *q = p + 2;
  return q[-1] - p[1];
}
// CHECK-LABEL: func.func @shared
// CHECK: emitrust.variable const <[97 : i8, 98 : i8, 99 : i8, 0 : i8]> : !emitrust.lvalue<!emitrust.array<4xi8>>
// CHECK-NOT: emitrust.variable const
// CHECK: emitrust.subscript
// CHECK: emitrust.subscript
// CHECK: arith.subi

// A literal-bound pointer feeds printf %s: the backing is sliced from the
// cursor and rendered by the same __emitrust_cstr helper as char arrays
// (both stop at the first NUL like C).
int printf(const char *, ...);
void print_it(void) {
  char *p = "hi";
  printf("%s\n", p + 1);
}
// CHECK-LABEL: func.func @print_it
// CHECK: %[[SLICE:.*]] = emitrust.slice_of %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<3xi8>>, i64) -> !emitrust.ref<!emitrust.slice<i8>>
// CHECK: %[[STR:.*]] = emitrust.call_opaque "__emitrust_cstr"(%[[SLICE]])
// CHECK: emitrust.call_opaque "print!"(%[[STR]])

// A definition-less strlen call over a literal-bound pointer counts bytes
// up to the first NUL through the __emitrust_strlen helper, converted to
// the call's declared result type.
int strlen(char *);
int len(void) {
  char *p = "hello";
  return strlen(p);
}
// CHECK-LABEL: func.func @len
// CHECK: %[[LSLICE:.*]] = emitrust.slice_of %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<6xi8>>, i64) -> !emitrust.ref<!emitrust.slice<i8>>
// CHECK: %[[COUNT:.*]] = emitrust.call_opaque "__emitrust_strlen"(%[[LSLICE]]) : (!emitrust.ref<!emitrust.slice<i8>>) -> i64
// CHECK: arith.trunci %[[COUNT]] : i64 to i32

// A string literal compared against a null pointer constant folds: the
// literal's address is never null in C.
int not_null(void) {
  return "abc" == (void *)0;
}
// CHECK-LABEL: func.func @not_null
// CHECK: %[[FALSE:.*]] = arith.constant false
// CHECK: arith.extui %[[FALSE]] : i1 to i32

// The helpers are emitted once per module, after all imported items.
// CHECK: emitrust.verbatim "fn __emitrust_cstr(s: &[i8]) -> String
// CHECK: emitrust.verbatim "fn __emitrust_strlen(s: &[i8]) -> i64
