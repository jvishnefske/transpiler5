// RUN: emitrust-import-c %s | FileCheck %s

struct Point {
  int x;
  int y;
};

long of_types(void) {
  // sizeof/_Alignof constant-fold at import time from clang's target
  // layout; the result has C type size_t (unsigned long -> ui64) and the
  // surrounding conversions are clang's usual implicit casts.
  return sizeof(char) + sizeof(short) + sizeof(int) + sizeof(long) +
         sizeof(double) + _Alignof(long);
}

// CHECK-LABEL: func.func @of_types
// CHECK: emitrust.constant <1 : ui64> : ui64
// CHECK: emitrust.constant <2 : ui64> : ui64
// CHECK: emitrust.constant <4 : ui64> : ui64
// CHECK: emitrust.constant <8 : ui64> : ui64
// CHECK: emitrust.cast %{{.*}} : ui64 to i64

int helper(void);

int of_expressions(int n) {
  int values[10];
  values[0] = n;
  // The operand of sizeof is unevaluated: helper() must not be called.
  // The ui64 result is converted to int by clang's implicit cast.
  int elem = sizeof helper();
  int whole = sizeof values;
  return elem + whole;
}

// CHECK-LABEL: func.func @of_expressions
// 4 (int element) and 40 (whole array), folded; the call is never emitted.
// CHECK-NOT: call @helper
// CHECK: emitrust.constant <4 : ui64> : ui64
// CHECK: emitrust.cast %{{.*}} : ui64 to i32
// CHECK: emitrust.constant <40 : ui64> : ui64
// CHECK-NOT: call @helper

int of_struct(void) {
  // Struct layout comes from the C target ABI: two ints -> 8 bytes.
  return (int)sizeof(struct Point);
}

// CHECK-LABEL: func.func @of_struct
// CHECK: emitrust.constant <8 : ui64> : ui64
