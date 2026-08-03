// RUN: emitrust-import-c %s | FileCheck %s

// C99-42 pin: nested struct members (both named types), whole-struct
// assignment (load + assign of the aggregate, i.e. value/Copy semantics),
// member-of-nested-member assignment, and whole-struct assignment through
// a pointer deref.

struct Inner {
  int x;
  int y;
};

struct Outer {
  struct Inner inner;
  int tag;
};

// Both struct_defs are emitted, and Outer's first member is the named
// nested struct type itself (by value, not a reference).
// CHECK: emitrust.struct_def @Inner ["x", "y"] [i32, i32]
// CHECK: emitrust.struct_def @Outer ["inner", "tag"] [!emitrust.struct<"Inner">, i32]

int copy_whole(void) {
  struct Outer s1;
  struct Outer s2;
  s2.inner.x = 1;
  s2.inner.y = 2;
  s2.tag = 3;
  s1 = s2;
  s1.inner.x = 10;
  return s1.inner.x + s2.inner.x + s1.tag;
}

// CHECK-LABEL: func.func @copy_whole
// CHECK: %[[S1:.*]] = emitrust.variable named "s1" : !emitrust.lvalue<!emitrust.struct<"Outer">>
// CHECK: %[[S2:.*]] = emitrust.variable named "s2" : !emitrust.lvalue<!emitrust.struct<"Outer">>

// Writing s2.inner.x is a two-hop member chain ending in an i32 place.
// CHECK: %[[IN1:.*]] = emitrust.member %[[S2]]["inner"] : (!emitrust.lvalue<!emitrust.struct<"Outer">>) -> !emitrust.lvalue<!emitrust.struct<"Inner">>
// CHECK: %[[X1:.*]] = emitrust.member %[[IN1]]["x"] : (!emitrust.lvalue<!emitrust.struct<"Inner">>) -> !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[X1]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK: emitrust.member %{{.*}}["y"]
// CHECK: emitrust.assign
// CHECK: emitrust.member %[[S2]]["tag"]
// CHECK: emitrust.assign

// s1 = s2 is a whole-aggregate load of s2's place assigned into s1's
// place: copy semantics, no aliasing.
// CHECK: %[[WHOLE:.*]] = emitrust.load %[[S2]] : (!emitrust.lvalue<!emitrust.struct<"Outer">>) -> !emitrust.struct<"Outer">
// CHECK: emitrust.assign %[[S1]] = %[[WHOLE]] : !emitrust.lvalue<!emitrust.struct<"Outer">>

// Mutating the copy targets s1's own nested place, leaving s2 untouched.
// CHECK: %[[IN2:.*]] = emitrust.member %[[S1]]["inner"]
// CHECK: %[[X2:.*]] = emitrust.member %[[IN2]]["x"]
// CHECK: emitrust.assign %[[X2]]

// The return reads both copies independently: s1.inner.x then s2.inner.x.
// CHECK: emitrust.member %[[S1]]["inner"]
// CHECK: emitrust.load
// CHECK: emitrust.member %[[S2]]["inner"]
// CHECK: emitrust.load
// CHECK: arith.addi
// CHECK: emitrust.member %[[S1]]["tag"]
// CHECK: arith.addi
// CHECK: return

void through_ptr(struct Outer *p, struct Outer s2) {
  *p = s2;
  p->inner.y = 5;
}

// Whole-struct assignment through a pointer deref: the by-value parameter
// is staged into a local place, *p becomes a deref place, and the whole
// aggregate is loaded and assigned into it. Then p->inner.y chains
// deref + member + member.
// CHECK-LABEL: func.func @through_ptr
// CHECK-SAME: (%[[P:.*]]: !emitrust.mut_ref<!emitrust.struct<"Outer">>, %[[ARG:.*]]: !emitrust.struct<"Outer">)
// CHECK: %[[LOC:.*]] = emitrust.variable named "s2" : !emitrust.lvalue<!emitrust.struct<"Outer">>
// CHECK: emitrust.assign %[[LOC]] = %[[ARG]] : !emitrust.lvalue<!emitrust.struct<"Outer">>
// CHECK: %[[PL:.*]] = emitrust.deref %[[P]] : (!emitrust.mut_ref<!emitrust.struct<"Outer">>) -> !emitrust.lvalue<!emitrust.struct<"Outer">>
// CHECK: %[[V:.*]] = emitrust.load %[[LOC]] : (!emitrust.lvalue<!emitrust.struct<"Outer">>) -> !emitrust.struct<"Outer">
// CHECK: emitrust.assign %[[PL]] = %[[V]] : !emitrust.lvalue<!emitrust.struct<"Outer">>
// CHECK: %[[PL2:.*]] = emitrust.deref %[[P]]
// CHECK: %[[PIN:.*]] = emitrust.member %[[PL2]]["inner"]
// CHECK: %[[PY:.*]] = emitrust.member %[[PIN]]["y"]
// CHECK: emitrust.assign %[[PY]]
// CHECK: return

int drive(void) {
  struct Outer a;
  struct Outer b;
  b.inner.x = 7;
  b.inner.y = 8;
  b.tag = 9;
  through_ptr(&a, b);
  return a.inner.y + b.inner.y;
}

// The caller passes &a as a mut_ref and b by value (a whole-aggregate
// load), then reads both structs' nested members independently.
// CHECK-LABEL: func.func @drive
// CHECK: %[[A:.*]] = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.struct<"Outer">>
// CHECK: %[[B:.*]] = emitrust.variable named "b" : !emitrust.lvalue<!emitrust.struct<"Outer">>
// CHECK: %[[BV:.*]] = emitrust.load %[[B]] : (!emitrust.lvalue<!emitrust.struct<"Outer">>) -> !emitrust.struct<"Outer">
// CHECK: %[[AREF:.*]] = emitrust.addr_of mut %[[A]] : (!emitrust.lvalue<!emitrust.struct<"Outer">>) -> !emitrust.mut_ref<!emitrust.struct<"Outer">>
// CHECK: call @through_ptr(%[[AREF]], %[[BV]])
// CHECK: emitrust.member %[[A]]["inner"]
// CHECK: emitrust.member %[[B]]["inner"]
// CHECK: return
