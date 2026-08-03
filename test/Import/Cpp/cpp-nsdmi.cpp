// RUN: emitrust-import-c %s | FileCheck %s

// A non-static data member initializer (NSDMI) makes the class's default
// constructor non-trivial, so `D d;` is a real 0-argument default
// construction. That constructor is never imported as a function; the
// importer applies each member's in-class initializer to the place directly,
// so `D d;` becomes field-wise assignment of the NSDMI constants. It used to
// reject as "call to an unimported constructor 'D_new'".

struct D {
  int x = 5;
  int y = 7;
};

int use() {
  D d;
  return d.x + d.y;
}

// CHECK: emitrust.struct_def @D ["x", "y"] [i32, i32]
// CHECK:      %[[D:.*]] = emitrust.variable named "d" : !emitrust.lvalue<!emitrust.struct<"D">>
// CHECK-NEXT: %[[MX:.*]] = emitrust.member %[[D]]["x"]
// CHECK-NEXT: %[[C5:.*]] = arith.constant 5 : i32
// CHECK-NEXT: emitrust.assign %[[MX]] = %[[C5]] : !emitrust.lvalue<i32>
// CHECK-NEXT: %[[MY:.*]] = emitrust.member %[[D]]["y"]
// CHECK-NEXT: %[[C7:.*]] = arith.constant 7 : i32
// CHECK-NEXT: emitrust.assign %[[MY]] = %[[C7]] : !emitrust.lvalue<i32>
