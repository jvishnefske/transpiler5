// RUN: emitrust-import-c %s | FileCheck %s

// C99-29: every float literal spelling — decimal, leading/trailing dot,
// exponent, hexadecimal (C99 6.4.4.2 hex floats), and the f/F suffixes —
// reaches the importer as clang's already-evaluated APFloat, so the
// spelling never survives into the IR: only the value and the type
// (float -> f32, double -> f64) do. Long double maps to f64 under the
// CTS 00204 policy, L-suffixed literals included (see long-double-f64.c
// and long-double-f64-invalid.c for the remaining boundaries).

// Hexadecimal double literals: 0x1.8p3 = 1.5 * 8 = 12.0, 0x1p-2 = 0.25,
// 0xA.8p0 = 10.5. All are exact binary values.
double hex_doubles(void) {
  double a = 0x1.8p3;
  double b = 0x1p-2;
  return a + b + 0xA.8p0;
}
// CHECK-LABEL: func.func @hex_doubles
// CHECK-DAG: arith.constant 1.200000e+01 : f64
// CHECK-DAG: arith.constant 2.500000e-01 : f64
// CHECK-DAG: arith.constant 1.050000e+01 : f64

// Hexadecimal float literals: the f suffix types the literal float, so
// the constant is f32. 0x1.8p1f = 3.0f, 0X1P4F (capital X/P/F) = 16.0f.
float hex_floats(void) {
  float a = 0x1.8p1f;
  return a + 0X1P4F;
}
// CHECK-LABEL: func.func @hex_floats
// CHECK-DAG: arith.constant 3.000000e+00 : f32
// CHECK-DAG: arith.constant 1.600000e+01 : f32

// Decimal spellings: leading dot, trailing dot, exponent forms (with
// signed exponents and capital E), and the F suffix.
double decimal_forms(void) {
  double a = .5;
  double b = 5.;
  double c = 1e3;
  double d = 2.5E-2;
  float e = 1.5F;
  float f = .25f;
  return a + b + c + d + e + f;
}
// CHECK-LABEL: func.func @decimal_forms
// CHECK-DAG: arith.constant 5.000000e-01 : f64
// CHECK-DAG: arith.constant 5.000000e+00 : f64
// CHECK-DAG: arith.constant 1.000000e+03 : f64
// CHECK-DAG: arith.constant 2.500000e-02 : f64
// CHECK-DAG: arith.constant 1.500000e+00 : f32
// CHECK-DAG: arith.constant 2.500000e-01 : f32

// A float literal in an unsuffixed context is double; assigning it to a
// float goes through clang's implicit FloatingCast, which the importer
// emits as arith.truncf — the literal itself stays f64.
float narrowed(void) {
  float f = 0.1;
  return f;
}
// CHECK-LABEL: func.func @narrowed
// CHECK: %[[D:.*]] = arith.constant 1.000000e-01 : f64
// CHECK: arith.truncf %[[D]] : f64 to f32

// A file-scope initializer folds through the APValue path: the hex
// spelling folds to the same FloatAttr a decimal spelling would.
double g_hex = 0x1.4p2;
// CHECK: emitrust.global @g_hex <5.000000e+00 : f64> : f64
