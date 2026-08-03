// RUN: emitrust-import-c %s | FileCheck %s

// C99-28 completion: string literals beyond the initializer and printf
// positions — expression-position element reads, sizeof, adjacent-literal
// concatenation, the full simple/octal/hex escape set, and unsigned char
// element types. The ASCII-only policy on literal bytes is unchanged
// (see strings-invalid.c).

// A file-scope unsigned char array takes a string initializer exactly like
// a plain char array; the elements are the unsigned ui8 domain.
unsigned char ug[] = "hi";
// CHECK: emitrust.global @ug <[104 : ui8, 105 : ui8, 0 : ui8]> : !emitrust.array<3xui8>

// Adjacent string literals concatenate in translation phase 6 (clang folds
// them into one StringLiteral before import); the array fills with the
// joined bytes plus the NUL, remaining elements keeping the zero fill.
int concat(void) {
  char cat[6] = "ab" "cd";
  return cat[3];
}
// CHECK-LABEL: func.func @concat
// CHECK: emitrust.variable named "cat" : !emitrust.lvalue<!emitrust.array<6xi8>>
// CHECK: arith.constant 97 : i8
// CHECK: arith.constant 98 : i8
// CHECK: arith.constant 99 : i8
// CHECK: arith.constant 100 : i8
// CHECK: arith.constant 0 : i8

// A block-scope unsigned char array initializer assigns ui8 constants
// through the emitrust.constant unsigned domain (bytes are bytes; the
// ASCII policy keeps every value in 0..=127 for both element types).
int ubytes(void) {
  unsigned char u[4] = "ab";
  return u[0];
}
// CHECK-LABEL: func.func @ubytes
// CHECK: emitrust.variable named "u" : !emitrust.lvalue<!emitrust.array<4xui8>>
// CHECK: emitrust.constant <97 : ui8> : ui8
// CHECK: emitrust.constant <98 : ui8> : ui8
// CHECK: emitrust.constant <0 : ui8> : ui8

// The full C escape set arrives from clang already decoded to bytes; the
// per-byte machinery is spelling-blind, so \a \b \f \v \r \? and the
// octal/hex forms land as their byte values (7 8 12 11 13 63 65 65).
int escapes(void) {
  char e[9] = "\a\b\f\v\r\?\x41\101";
  return e[0];
}
// CHECK-LABEL: func.func @escapes
// CHECK: arith.constant 7 : i8
// CHECK: arith.constant 8 : i8
// CHECK: arith.constant 12 : i8
// CHECK: arith.constant 11 : i8
// CHECK: arith.constant 13 : i8
// CHECK: arith.constant 63 : i8
// CHECK: arith.constant 65 : i8
// CHECK: arith.constant 65 : i8
// CHECK: arith.constant 0 : i8

// sizeof of a string literal is the array size — length plus the
// terminating NUL (C99 6.5.3.4), folded through clang's layout query.
long size_of(void) {
  return sizeof("abc");
}
// CHECK-LABEL: func.func @size_of
// CHECK: emitrust.constant <4 : ui64> : ui64

// A subscript directly on a literal reads through the same cached
// read-only const backing a bound pointer would use; the backing holds
// the bytes plus the NUL.
int subscript(int i) {
  return "abc"[i];
}
// CHECK-LABEL: func.func @subscript
// CHECK: %[[BACK:.*]] = emitrust.variable const <[97 : i8, 98 : i8, 99 : i8, 0 : i8]> : !emitrust.lvalue<!emitrust.array<4xi8>>
// CHECK: %[[EL:.*]] = emitrust.subscript %[[BACK]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi8>>, i32) -> !emitrust.lvalue<i8>
// CHECK: emitrust.load %[[EL]] : (!emitrust.lvalue<i8>) -> i8

// The deref-of-arithmetic spelling decays the literal to (backing,
// cursor 0) on demand and reads the byte at the offset, exactly like a
// walking pointer bound to the literal (CTS-P1).
int deref(void) {
  return *("qr" + 1);
}
// CHECK-LABEL: func.func @deref
// CHECK: %[[QB:.*]] = emitrust.variable const <[113 : i8, 114 : i8, 0 : i8]> : !emitrust.lvalue<!emitrust.array<3xi8>>
// CHECK: emitrust.subscript %[[QB]][%{{.*}}]
// CHECK: emitrust.load
