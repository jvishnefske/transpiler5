// RUN: emitrust-import-c %s | FileCheck %s

// CTS-S4 / C99-41: multi-dimensional arrays import as nested
// !emitrust.array types, file-scope initializers (with designators and
// zero fill) as nested list attributes, and each subscript level peels one
// array dimension (row-major).

int grid[2][3][5] = {
    {{0, 0, 3, 5}, {1, [3] = 6, 7}},
    {{1, 2}, {[4] = 7}},
};

// Designators resolve and the holes zero-fill, level by level.
// CHECK: emitrust.global @grid <[
// CHECK-SAME: [0 : i32, 0 : i32, 3 : i32, 5 : i32, 0 : i32]
// CHECK-SAME: [1 : i32, 0 : i32, 0 : i32, 6 : i32, 7 : i32]
// CHECK-SAME: ]> : !emitrust.array<2x!emitrust.array<3x!emitrust.array<5xi32>>>

int sum2d(int i, int j) {
  int a[2][4] = {{1, 2, 3, 4}, {5}};
  a[1][3] = a[0][1] + 1;
  return a[i][j];
}

// The local is one place of the nested array type; the initializer writes
// element places level by level (unwritten elements keep the default zero).
// CHECK-LABEL: func.func @sum2d
// CHECK: %[[A:.*]] = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.array<2x!emitrust.array<4xi32>>>
// CHECK: %[[ROW0:.*]] = emitrust.subscript %[[A]][{{.*}}] : (!emitrust.lvalue<!emitrust.array<2x!emitrust.array<4xi32>>>, i64) -> !emitrust.lvalue<!emitrust.array<4xi32>>
// CHECK: emitrust.subscript %[[ROW0]][{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>
// CHECK: emitrust.assign

// The write and the data-dependent read both chain one subscript per level.
// CHECK: %[[ROW1:.*]] = emitrust.subscript %[[A]][{{.*}}] : (!emitrust.lvalue<!emitrust.array<2x!emitrust.array<4xi32>>>, i32) -> !emitrust.lvalue<!emitrust.array<4xi32>>
// CHECK: %[[ELT:.*]] = emitrust.subscript %[[ROW1]][{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
// CHECK: emitrust.load
// CHECK: return

int through_pointers(void) {
  char arr[2][4];
  char (*p)[4] = arr;
  char *q = &arr[1][3];
  arr[1][3] = 2;
  return *q + p[1][2];
}

// `q` holds the flat row-major cursor 1*4 + 3; `*q` peels the two array
// levels with a div/rem pair. `p` counts rows through its flat cursor:
// `p[1]` scales the index by the row span (4) and divides it back out to
// reach the row place, then `[2]` subscripts the row directly.
// CHECK-LABEL: func.func @through_pointers
// CHECK: %[[ARR:.*]] = emitrust.variable named "arr" : !emitrust.lvalue<!emitrust.array<2x!emitrust.array<4xi8>>>
// CHECK: arith.muli {{.*}} : i64
// CHECK: arith.addi {{.*}} : i64
// CHECK: %[[QDIV:.*]] = arith.divsi
// CHECK: %[[QREM:.*]] = arith.remsi
// CHECK: %[[QROW:.*]] = emitrust.subscript %[[ARR]][%[[QDIV]]] : (!emitrust.lvalue<!emitrust.array<2x!emitrust.array<4xi8>>>, i64) -> !emitrust.lvalue<!emitrust.array<4xi8>>
// CHECK: emitrust.subscript %[[QROW]][%[[QREM]]] : (!emitrust.lvalue<!emitrust.array<4xi8>>, i64) -> !emitrust.lvalue<i8>
// CHECK: %[[PDIV:.*]] = arith.divsi
// CHECK-NOT: arith.remsi
// CHECK: %[[PROW:.*]] = emitrust.subscript %[[ARR]][%[[PDIV]]] : (!emitrust.lvalue<!emitrust.array<2x!emitrust.array<4xi8>>>, i64) -> !emitrust.lvalue<!emitrust.array<4xi8>>
// CHECK: emitrust.subscript %[[PROW]][{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi8>>, i32) -> !emitrust.lvalue<i8>
// CHECK: return

struct Point {
  int x;
  int y;
};

int place_chain(int i, int j) {
  struct Point cells[2][3];
  cells[i][j].x = 5;
  return cells[1][2].y + cells[i][j].x;
}

// Full place-chain access (C99-41): subscript levels compose with member
// refinement on an array-of-arrays-of-structs.
// CHECK-LABEL: func.func @place_chain
// CHECK: %[[CELLS:.*]] = emitrust.variable named "cells" : !emitrust.lvalue<!emitrust.array<2x!emitrust.array<3x!emitrust.struct<"Point">>>>
// CHECK: %[[CROW:.*]] = emitrust.subscript %[[CELLS]][{{.*}}] : (!emitrust.lvalue<!emitrust.array<2x!emitrust.array<3x!emitrust.struct<"Point">>>>, i32) -> !emitrust.lvalue<!emitrust.array<3x!emitrust.struct<"Point">>>
// CHECK: %[[CELL:.*]] = emitrust.subscript %[[CROW]][{{.*}}] : (!emitrust.lvalue<!emitrust.array<3x!emitrust.struct<"Point">>>, i32) -> !emitrust.lvalue<!emitrust.struct<"Point">>
// CHECK: emitrust.member %[[CELL]]["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
// CHECK: emitrust.assign
// CHECK: emitrust.member %{{.*}}["y"]
// CHECK: return
