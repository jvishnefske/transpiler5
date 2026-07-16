// RUN: emitrust-import-c %s | FileCheck %s

// C99-11/C99-12: aggregate initializer lists for arrays and structs at both
// scopes, including partial initialization with implicit zeroing, `[i] =`
// and `.field =` designators, and nested array-of-struct /
// struct-with-array lists.

struct P {
  int x;
  double d;
};

struct WithArr {
  int tag;
  int vals[3];
};

// A file-scope array initializer becomes a typed element list on the
// global; the hole left by the designator jump is zero-filled.
// CHECK: emitrust.global @garr <[1 : i32, -2 : i32, 0 : i32, 7 : i32]> : !emitrust.array<4xi32>
int garr[4] = {1, -2, [3] = 7};

// A never-written const array stays a `const` global with its list.
// CHECK: emitrust.global const @gconst <[5 : i32, 6 : i32]> : !emitrust.array<2xi32>
const int gconst[2] = {5, 6};

// A designated struct initializer zero-fills the unnamed fields.
// CHECK: emitrust.global @gp <[0 : i32, 1.500000e+00]> : !emitrust.struct<"P">
struct P gp = {.d = 1.5};

// Array-of-struct: nested lists; the trailing element is all-default.
// CHECK: emitrust.global @gnest <{{\[}}[1 : i32, 2.000000e+00], [0 : i32, 0.000000e+00]]> : !emitrust.array<2x!emitrust.struct<"P">>
struct P gnest[2] = {{1, 2.0}};

// Struct-with-array: the array field is a nested element list.
// CHECK: emitrust.global @gwa <[9 : i32, [-1 : i32, 0 : i32, 4 : i32]]> : !emitrust.struct<"WithArr">
struct WithArr gwa = {9, {-1, [2] = 4}};

// Unsigned elements keep their unsigned type and full width.
// CHECK: emitrust.global @gu <[4000000000 : ui32, 0 : ui32]> : !emitrust.array<2xui32>
unsigned int gu[2] = {4000000000u};

int use_globals(void) {
  return garr[0] + gconst[1] + gp.x + gnest[0].x + gwa.tag + (int)gu[1];
}

// A block-scope list keeps the default-initialized variable (C99
// zero-fill) and assigns each explicit element through a constant-index
// subscript place; the hole at index 2 gets no assignment.
// CHECK-LABEL: func.func @local_array
// CHECK: %[[A:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
// CHECK-DAG: %[[I0:.*]] = arith.constant 0 : i64
// CHECK: %[[E0:.*]] = emitrust.subscript %[[A]][%[[I0]]] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[E0]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK: %[[I1:.*]] = arith.constant 1 : i64
// CHECK: %[[E1:.*]] = emitrust.subscript %[[A]][%[[I1]]]
// CHECK: emitrust.assign %[[E1]]
// CHECK: %[[I3:.*]] = arith.constant 3 : i64
// CHECK: %[[E3:.*]] = emitrust.subscript %[[A]][%[[I3]]]
// CHECK: emitrust.assign %[[E3]]
// CHECK-NOT: arith.constant 2 : i64
int local_array(int n) {
  int a[4] = {-1, n * 2, [3] = 8};
  return a[0] + a[2];
}

// A block-scope struct list assigns the designated fields through member
// places; the undesignated field keeps the default.
// CHECK-LABEL: func.func @local_struct
// CHECK: %[[S:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"P">>
// CHECK: %[[FX:.*]] = emitrust.member %[[S]]["x"]
// CHECK: emitrust.assign %[[FX]]
// CHECK-NOT: emitrust.member %[[S]]["d"]
int local_struct(int n) {
  struct P p = {.x = n + 1};
  return p.x;
}

// Nested block-scope lists recurse: member place, then subscripts into it.
// CHECK-LABEL: func.func @local_nested
// CHECK: %[[W:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"WithArr">>
// CHECK: %[[TAG:.*]] = emitrust.member %[[W]]["tag"]
// CHECK: emitrust.assign %[[TAG]]
// CHECK: %[[VALS:.*]] = emitrust.member %[[W]]["vals"] : (!emitrust.lvalue<!emitrust.struct<"WithArr">>) -> !emitrust.lvalue<!emitrust.array<3xi32>>
// CHECK: %[[V0:.*]] = emitrust.subscript %[[VALS]][%{{.*}}]
// CHECK: emitrust.assign %[[V0]]
int local_nested(int n) {
  struct WithArr w = {n, {n * 3}};
  return w.tag + w.vals[0];
}

// A function-local static with a list routes through the global path.
// CHECK: emitrust.global @local_static_seeds <[10 : i32, -20 : i32, 0 : i32]> : !emitrust.array<3xi32>
int local_static(void) {
  static int seeds[3] = {10, -20};
  return seeds[0];
}
