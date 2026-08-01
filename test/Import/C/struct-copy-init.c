// RUN: emitrust-import-c %s | FileCheck %s

// C copy-initialization of a struct object from another (`struct T y = x;`)
// is a whole-struct value copy: the source place is loaded whole and stored
// into the fresh variable, exactly as the assignment form `y = x;` is. It
// used to reject as "unsupported: aggregate initializer".

struct T {
  int a;
  int b;
};

int use(void) {
  struct T x = {3, 4};
  struct T y = x;
  return y.a + y.b;
}

// CHECK: emitrust.struct_def @T ["a", "b"] [i32, i32]
// CHECK:      %[[X:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"T">>
// The copy destination and the whole-struct load-then-store of the source.
// CHECK:      %[[Y:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"T">>
// CHECK-NEXT: %[[V:.*]] = emitrust.load %[[X]] : (!emitrust.lvalue<!emitrust.struct<"T">>) -> !emitrust.struct<"T">
// CHECK-NEXT: emitrust.assign %[[Y]] = %[[V]] : !emitrust.lvalue<!emitrust.struct<"T">>
