// RUN: emitrust-import-c %s | FileCheck %s

struct Point {
  int x;
  int y;
};

void set_origin(struct Point *p) {
  p->x = 0;
  p->y = 0;
}

int manhattan(struct Point q) {
  return q.x + q.y;
}

int use_point(void) {
  struct Point pt;
  pt.x = 3;
  pt.y = 4;
  set_origin(&pt);
  return manhattan(pt) + pt.x;
}

// CHECK: emitrust.struct_def @Point ["x", "y"] [i32, i32]

// Writing through a struct pointer parameter: deref + member + assign.
// CHECK-LABEL: func.func @set_origin
// CHECK-SAME: (%[[P:.*]]: !emitrust.mut_ref<!emitrust.struct<"Point">>)
// CHECK: %[[PL:.*]] = emitrust.deref %[[P]] : (!emitrust.mut_ref<!emitrust.struct<"Point">>) -> !emitrust.lvalue<!emitrust.struct<"Point">>
// CHECK: %[[PX:.*]] = emitrust.member %[[PL]]["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[PX]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK: emitrust.member %{{.*}}["y"]
// CHECK: emitrust.assign
// CHECK: return

// A by-value struct parameter is copied into a local variable place.
// CHECK-LABEL: func.func @manhattan
// CHECK-SAME: (%[[Q:.*]]: !emitrust.struct<"Point">) -> i32
// CHECK: %[[QV:.*]] = emitrust.variable named "q" : !emitrust.lvalue<!emitrust.struct<"Point">>
// CHECK: emitrust.assign %[[QV]] = %[[Q]] : !emitrust.lvalue<!emitrust.struct<"Point">>
// CHECK: emitrust.member %[[QV]]["x"]
// CHECK: emitrust.load
// CHECK: arith.addi
// CHECK: return

// Struct local, field writes, address-of for a pointer argument, and a
// by-value load for a struct argument.
// CHECK-LABEL: func.func @use_point
// CHECK: %[[PT:.*]] = emitrust.variable named "pt" : !emitrust.lvalue<!emitrust.struct<"Point">>
// CHECK: emitrust.member %[[PT]]["x"]
// CHECK: emitrust.assign
// CHECK: emitrust.member %[[PT]]["y"]
// CHECK: emitrust.assign
// CHECK: %[[REF:.*]] = emitrust.addr_of mut %[[PT]] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.mut_ref<!emitrust.struct<"Point">>
// CHECK: call @set_origin(%[[REF]])
// CHECK: %[[ARG:.*]] = emitrust.load %[[PT]] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.struct<"Point">
// CHECK: call @manhattan(%[[ARG]])
// CHECK: return
