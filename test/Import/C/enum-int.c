// RUN: emitrust-import-c %s | FileCheck %s

// C99-5: mixed enum/int comparisons and conversions. C converts the enum
// operand to the common integer type; the importer renders that conversion
// as an `emitrust.cast` from the enum to the mapped integer type (the
// open-enum tuple struct exposes its raw value through field `0`, rendered
// `.0 as`). Color has no negative enumerator, so clang gives it an
// unsigned underlying type and mixed comparisons happen at ui32 through
// the type-directed `emitrust.cmp`; Temp's negative enumerator forces a
// signed underlying type and mixed comparisons happen at signless i32
// through `arith.cmpi`.

enum Color { Red, Green = 5, Blue };
enum Temp { Cold = -1, Warm = 1 };

int mixed(enum Color c, enum Temp t) {
  int score = 0;
  if (c == 5) {
    score = 1;
  }
  if (0 == c) {
    score = 2;
  }
  if (t > -1) {
    score = 3;
  }
  int direct = (int)c;
  return score + direct;
}

// CHECK-LABEL: func.func @mixed

// enum == int: the enum converts to its unsigned underlying type alongside
// the promoted literal, and the comparison is unsigned.
// CHECK: %[[C1:.*]] = emitrust.cast %{{[0-9]+}} : !emitrust.enum<"Color"> to ui32
// CHECK: %[[L1:.*]] = emitrust.cast %{{.*}} : i32 to ui32
// CHECK: emitrust.cmp eq, %[[C1]], %[[L1]] : (ui32, ui32) -> i1

// int == enum: the same conversion applies with the operands swapped.
// CHECK: %[[L2:.*]] = emitrust.cast %{{.*}} : i32 to ui32
// CHECK: %[[C2:.*]] = emitrust.cast %{{[0-9]+}} : !emitrust.enum<"Color"> to ui32
// CHECK: emitrust.cmp eq, %[[L2]], %[[C2]] : (ui32, ui32) -> i1

// enum (signed underlying) relational against a negative literal: both
// sides are signless i32 and the comparison is signed.
// CHECK: %[[T:.*]] = emitrust.cast %{{[0-9]+}} : !emitrust.enum<"Temp"> to i32
// CHECK: arith.cmpi sgt, %[[T]], %{{[0-9]+}} : i32

// Explicit (int)e cast.
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"Color"> to i32
// CHECK: return
