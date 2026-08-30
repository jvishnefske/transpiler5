// RUN: emitrust-import-c %s | FileCheck %s

// FR-169. Pins the SIGNEDNESS and WIDTH of the integer an enum-typed value
// is normalized to before a relational comparison or a floating conversion.
//
// The rule is C17 6.7.2.2: an enumerated type is compatible with an
// implementation-chosen integer type, and clang picks an UNSIGNED one when
// no enumerator is negative. So an enum-typed OBJECT of such a type may
// hold any value of the unsigned range -- the i32-range guard on the
// importer's side bounds ENUMERATOR values, not object values -- and
// `<`/`<=`/`>`/`>=` on it must compare unsigned. The signedness comes from
// the `unsigned_underlying` marker recorded on `emitrust.enum_def`, NOT
// from clang's promoted type: clang promotes such an enum to `unsigned int`
// even when every enumerator fits `int`, so a promoted-type-driven rule
// would move bytes for every non-negative enum in the corpus.
//
// Unsigned goes to `ui32`/`ui64` and an `emitrust.cmp` (arith has no
// unsigned-typed cmpi); signed keeps the historical `i32`/`i64` +
// `arith.cmpi slt` shape byte for byte. Equality is signedness-neutral and
// compares the enum values directly at every flavor.
//
// The switch discriminant and the subscript index deliberately stay on the
// signed `castEnumToI32` path -- both were measured bit-preserving -- so
// this file also pins that FR-169 did NOT widen its blast radius to them.

int printf(const char *, ...);

enum U32 { U32_A = 0, U32_B = 2000000000 };
enum S32 { S32_A = -3, S32_B = 2000000000 };
enum WU { WU_A = 0, WU_B = 5000000000ULL };
enum WS { WS_A = -1, WS_B = 5000000000LL };

// CHECK: emitrust.enum_def @U32 ["U32_A", "U32_B"] [0, 2000000000] {unsigned_underlying}
// CHECK: emitrust.enum_def @S32 ["S32_A", "S32_B"] [-3, 2000000000]
// CHECK: emitrust.enum_def @WU ["WU_A", "WU_B"] [0, 5000000000] {unsigned_underlying, wide_underlying}
// CHECK: emitrust.enum_def @WS ["WS_A", "WS_B"] [-1, 5000000000] {wide_underlying}

// A 32-bit unsigned-underlying enum promotes to `ui32` and compares with
// `emitrust.cmp`, which lowers to Rust's signedness-carrying `<`.
// CHECK-LABEL: func.func @lt_u32
// CHECK: %[[UL:.*]] = emitrust.cast %{{[0-9]+}} : !emitrust.enum<"U32"> to ui32
// CHECK: %[[UR:.*]] = emitrust.cast %{{[0-9]+}} : !emitrust.enum<"U32"> to ui32
// CHECK: emitrust.cmp lt, %[[UL]], %[[UR]] : (ui32, ui32) -> i1
int lt_u32(enum U32 a, enum U32 b) { return a < b; }

// CHECK-LABEL: func.func @ge_u32
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"U32"> to ui32
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"U32"> to ui32
// CHECK: emitrust.cmp ge
int ge_u32(enum U32 a, enum U32 b) { return a >= b; }

// A negative enumerator forces a signed underlying type: unchanged shape.
// CHECK-LABEL: func.func @lt_s32
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"S32"> to i32
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"S32"> to i32
// CHECK: arith.cmpi slt
int lt_s32(enum S32 a, enum S32 b) { return a < b; }

// CHECK-LABEL: func.func @le_s32
// CHECK: arith.cmpi sle
int le_s32(enum S32 a, enum S32 b) { return a <= b; }

// FR-166's 64-bit storage follows the same rule at `ui64`.
// CHECK-LABEL: func.func @gt_wu
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"WU"> to ui64
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"WU"> to ui64
// CHECK: emitrust.cmp gt, %{{[0-9]+}}, %{{[0-9]+}} : (ui64, ui64) -> i1
int gt_wu(enum WU a, enum WU b) { return a > b; }

// CHECK-LABEL: func.func @gt_ws
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"WS"> to i64
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"WS"> to i64
// CHECK: arith.cmpi sgt
int gt_ws(enum WS a, enum WS b) { return a > b; }

// Equality never went through the integer normalization at all.
// CHECK-LABEL: func.func @eq_u32
// CHECK-NOT: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"U32"> to
// CHECK: emitrust.cmp eq, %{{[0-9]+}}, %{{[0-9]+}} : (!emitrust.enum<"U32">, !emitrust.enum<"U32">) -> i1
int eq_u32(enum U32 a, enum U32 b) { return a == b; }

// FR-169 Phase B: a floating destination normalizes the enum source the
// same way. Unsigned takes the `emitrust.cast`-to-float path (Rust's
// `u* as f*` rounds exactly like C's); signed keeps `arith.sitofp`.
// Both used to hand an enum-typed operand straight to `arith.sitofp` and
// die in the op verifier.
// CHECK-LABEL: func.func @to_double_u32
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"U32"> to ui32
// CHECK: emitrust.cast %{{[0-9]+}} : ui32 to f64
// CHECK-NOT: arith.sitofp
double to_double_u32(enum U32 a) { return (double)a; }

// CHECK-LABEL: func.func @to_double_s32
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"S32"> to i32
// CHECK: arith.sitofp
double to_double_s32(enum S32 a) { return (double)a; }

// The IMPLICIT initializer conversion, whose rejection carried no location.
// CHECK-LABEL: func.func @init_double_wu
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"WU"> to ui64
// CHECK: emitrust.cast %{{[0-9]+}} : ui64 to f64
double init_double_wu(enum WU a) {
  double d = a;
  return d;
}

// CHECK-LABEL: func.func @to_float_ws
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"WS"> to i64
// CHECK: arith.sitofp
float to_float_ws(enum WS a) { return (float)a; }

// Non-regression: the SWITCH discriminant and the SUBSCRIPT index keep the
// signed `castEnumToI32` normalization. Both are bit-preserving -- the
// switch flag reaches Rust through an `as i32 as u32 as usize` chain, and
// an out-of-bounds index is undefined behavior either way -- so FR-169
// deliberately leaves them alone rather than widening its blast radius.
// The truth test is listed here too, and it is NOT a change: clang inserts
// its own integral promotion to `unsigned int` ahead of
// `CK_IntegralToBoolean`, so this shape (`to ui32` + `emitrust.cmp ne`) is
// exactly what HEAD emitted before FR-169, and `!= 0` is signedness-neutral
// regardless.
// CHECK-LABEL: func.func @untouched
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"U32"> to ui32
// CHECK: emitrust.cmp ne
// CHECK: %[[SW:.*]] = emitrust.cast %{{[0-9]+}} : !emitrust.enum<"U32"> to i32
// CHECK: cf.switch %[[SW]] : i32, [
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"U32"> to i32
// CHECK: emitrust.subscript
int untouched(enum U32 a, int *tbl) {
  int r = a ? 1 : 0;
  switch (a) {
  case U32_A:
    r = r + 10;
    break;
  default:
    r = r + 20;
    break;
  }
  return r + tbl[a];
}
