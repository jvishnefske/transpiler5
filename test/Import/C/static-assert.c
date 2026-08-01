// RUN: emitrust-import-c %s | FileCheck %s

// A `_Static_assert` at file scope is a compile-time-only check the C
// frontend already evaluated; it produces no runtime code and is discarded
// (like a typedef), leaving the surrounding declarations to import normally.
// It used to reject as "unsupported top-level declaration".

_Static_assert(sizeof(int) == 4, "int must be 4 bytes");

typedef int my_int;
_Static_assert(sizeof(my_int) == 4, "");

int add(int a, int b) {
  return a + b;
}

// CHECK-NOT: static_assert
// CHECK: func.func @add(%{{.*}}: i32, %{{.*}}: i32) -> i32
