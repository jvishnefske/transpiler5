// RUN: emitrust-import-c %s | FileCheck %s

enum Color { Red, Green = 5, Blue };
enum { Bias = 40 };

int describe(enum Color c) {
  enum Color mine = Green;
  int score = 0;
  if (c == mine) {
    score = 1;
  }
  if (c != Red) {
    score = score + 1;
  }
  if (c < mine) {
    score = score + 2;
  }
  switch (c) {
  case Red:
    score = score + 3;
    break;
  case Green:
    score = score + 4;
    break;
  default:
    break;
  }
  int raw = (int)c;
  return score + raw + Bias + Green;
}

enum Color pick(enum Color a, enum Color b) {
  if (a == b) {
    return a;
  }
  return Blue;
}

// The complete named enum becomes a module-level definition with the C
// enumerator spellings and their (explicit and implicit) values; the
// anonymous enum contributes no definition. All enumerators are
// non-negative, so clang's unsigned underlying-type choice is recorded.
// CHECK: emitrust.enum_def @Color ["Red", "Green", "Blue"] [0, 5, 6] {unsigned_underlying}
// CHECK-NOT: emitrust.enum_def @

// Enum-typed parameters and locals are opaque enum values held in variable
// places, never memref cells.
// CHECK-LABEL: func.func @describe
// CHECK-SAME: (%[[C:.*]]: !emitrust.enum<"Color">) -> i32
// CHECK: %[[CV:.*]] = emitrust.variable named "c" : !emitrust.lvalue<!emitrust.enum<"Color">>
// CHECK: emitrust.assign %[[CV]] = %[[C]] : !emitrust.lvalue<!emitrust.enum<"Color">>
// CHECK: %[[MINE:.*]] = emitrust.variable named "mine" : !emitrust.lvalue<!emitrust.enum<"Color">>
// CHECK: emitrust.constant <#emitrust.opaque<"Color::Green">> : !emitrust.enum<"Color">
// CHECK: emitrust.assign %[[MINE]]

// Equality between enum values compares the enum type directly.
// CHECK: emitrust.cmp eq, %{{[0-9]+}}, %{{[0-9]+}} : (!emitrust.enum<"Color">, !emitrust.enum<"Color">) -> i1

// Inequality against an enumerator constant.
// CHECK: emitrust.constant <#emitrust.opaque<"Color::Red">> : !emitrust.enum<"Color">
// CHECK: emitrust.cmp ne, %{{[0-9]+}}, %{{[0-9]+}} : (!emitrust.enum<"Color">, !emitrust.enum<"Color">) -> i1

// Relational comparison has no derived Rust ordering; the i32 discriminants
// are compared instead.
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"Color"> to i32
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"Color"> to i32
// CHECK: arith.cmpi slt

// A switch over an enum switches on the i32 discriminant with the
// enumerator values as case values.
// CHECK: %[[FLAG:.*]] = emitrust.cast %{{[0-9]+}} : !emitrust.enum<"Color"> to i32
// CHECK: cf.switch %[[FLAG]] : i32, [
// CHECK-NEXT: default: ^bb{{[0-9]+}},
// CHECK-NEXT: 0: ^bb{{[0-9]+}},
// CHECK-NEXT: 5: ^bb{{[0-9]+}}
// CHECK-NEXT: ]

// The explicit enum-to-int cast and the enumerator used in integer
// arithmetic both render as an `as i32` cast; the anonymous enumerator is a
// plain i32 constant.
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"Color"> to i32
// CHECK: arith.constant 40 : i32
// CHECK: emitrust.constant <#emitrust.opaque<"Color::Green">> : !emitrust.enum<"Color">
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"Color"> to i32
// CHECK: return

// Enum values pass, load, and return through the existing machinery.
// CHECK-LABEL: func.func @pick
// CHECK-SAME: (%{{.*}}: !emitrust.enum<"Color">, %{{.*}}: !emitrust.enum<"Color">) -> !emitrust.enum<"Color">
// CHECK: emitrust.cmp eq
// CHECK: return %{{[0-9]+}} : !emitrust.enum<"Color">
// CHECK: emitrust.constant <#emitrust.opaque<"Color::Blue">> : !emitrust.enum<"Color">
// CHECK: return
