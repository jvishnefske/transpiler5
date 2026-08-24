// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s
// RUN: emitrust-import-c %s | FileCheck %s --check-prefix=MLIR

// FR-129 half (b): glibc's <ctype.h> CLASSIFIER family, admitted in BOOLEAN
// CONTEXT ONLY. `isspace(c)` is a macro that expands to
// `(*__ctype_b_loc())[(int)(c)] & _ISspace`, and what it yields is not 1 but
// the glibc `_IS*` MASK (8192 for space) -- C promises only "nonzero if
// true". A Rust image returning `bool` is therefore byte-identical to the
// native build EXACTLY where the nonzero value is unobservable: `if`/`while`/
// `for` conditions, the `!` operand, the `&&`/`||` operands, and the ternary
// condition. This pins that those contexts lower to the `__emitrust_is*`
// ASCII predicates over the argument's `u8` image, and that the emitted
// helpers say what they mean. Every OTHER use keeps the located ctype-table
// rejection (test/Import/C/ctype-classifier-invalid.c) and the exhaustive
// 256-value byte-diff lives in test/EndToEnd/ctype-classifiers.c.
//
// This test deliberately uses the REAL header: the shape being admitted IS
// glibc's macro expansion, and a hand-written stand-in could drift from it.
// (The half-(a) rejection test keeps its hand-written shape for the opposite
// reason -- it must pin a wording that does not depend on the host libc.)

#include <ctype.h>

// The argument reaches the predicate as its `unsigned char` image: C's own
// domain rule for these functions is "EOF, or representable as unsigned
// char", and `EOF as u8` is 255, which no ASCII classifier accepts -- the
// same answer glibc gives for EOF.
// CHECK-LABEL: pub fn if_space(c: i32) -> i32 {
// CHECK-NEXT:    let v1: bool = __emitrust_isspace(c as u8);
// MLIR: emitrust.call_opaque "__emitrust_isspace"
int if_space(int c) {
  if (isspace(c))
    return 1;
  return 0;
}

// `!` is a boolean context: the C value of `!isdigit(c)` is 0 or 1 no matter
// which nonzero the classifier produced.
// CHECK-LABEL: pub fn not_digit(c: i32) -> i32 {
// CHECK-NEXT:    let v2: bool = __emitrust_isdigit(c as u8);
// CHECK-NEXT:    (v2 ^ true) as i32
int not_digit(int c) {
  if (!isdigit(c))
    return 1;
  return 0;
}

// Both `&&` operands are boolean contexts, including the one in VALUE
// position: C's `&&` itself yields 0 or 1, so the mask never escapes.
// CHECK-LABEL: pub fn alpha_and_lower(c: i32) -> i32 {
// CHECK: __emitrust_isalpha(
// CHECK: __emitrust_islower(
int alpha_and_lower(int c) {
  return isalpha(c) && islower(c);
}

// CHECK-LABEL: pub fn upper_or_digit(c: i32) -> i32 {
// CHECK: __emitrust_isupper(
// CHECK: __emitrust_isdigit(
int upper_or_digit(int c) {
  return isupper(c) || isdigit(c);
}

// A ternary CONDITION is a boolean context; the arms are ordinary values.
// CHECK-LABEL: pub fn ternary_alnum(c: i32) -> i32 {
// CHECK: __emitrust_isalnum(
int ternary_alnum(int c) {
  return isalnum(c) ? 10 : 20;
}

// The measured corpus shape (inih's ini_lskip): a classifier as a `while`
// condition operand, over a `char` read through a cursor.
// CHECK-LABEL: pub fn skip_space(
// CHECK: __emitrust_isspace(s[v8 as usize] as u8 as i32 as u8)
int skip_space(const char *s) {
  int n = 0;
  while (*s && isspace((unsigned char)(*s))) {
    ++s;
    ++n;
  }
  return n;
}

// CHECK-LABEL: pub fn for_punct(c: i32) -> i32 {
// CHECK: __emitrust_ispunct(c as u8)
int for_punct(int c) {
  int n = 0;
  for (; ispunct(c) && n < 3; ++n)
    ;
  return n;
}

// The remaining six masks, each pinned to its own helper so a transposed
// table entry cannot hide behind a sibling.
// CHECK-LABEL: pub fn rest(c: i32) -> i32 {
// CHECK: __emitrust_isxdigit(
// CHECK: __emitrust_isblank(
// CHECK: __emitrust_iscntrl(
// CHECK: __emitrust_isprint(
// CHECK: __emitrust_isgraph(
int rest(int c) {
  int n = 0;
  if (isxdigit(c))
    n += 1;
  if (isblank(c))
    n += 2;
  if (iscntrl(c))
    n += 4;
  if (isprint(c))
    n += 8;
  if (isgraph(c))
    n += 16;
  return n;
}

// The emitted helpers, once per module and in mask order. `isspace` is
// spelled out rather than calling `is_ascii_whitespace`, which follows the
// WhatWG definition and excludes U+000B VERTICAL TAB -- the one value out of
// 256 where Rust's predicate and C's classifier disagree.
// CHECK: fn __emitrust_isblank(c: u8) -> bool {
// CHECK-NEXT:    matches!(c, b' ' | b'\t')
// CHECK: fn __emitrust_iscntrl(c: u8) -> bool {
// CHECK-NEXT:    c.is_ascii_control()
// CHECK: fn __emitrust_ispunct(c: u8) -> bool {
// CHECK-NEXT:    c.is_ascii_punctuation()
// CHECK: fn __emitrust_isalnum(c: u8) -> bool {
// CHECK-NEXT:    c.is_ascii_alphanumeric()
// CHECK: fn __emitrust_isupper(c: u8) -> bool {
// CHECK-NEXT:    c.is_ascii_uppercase()
// CHECK: fn __emitrust_islower(c: u8) -> bool {
// CHECK-NEXT:    c.is_ascii_lowercase()
// CHECK: fn __emitrust_isalpha(c: u8) -> bool {
// CHECK-NEXT:    c.is_ascii_alphabetic()
// CHECK: fn __emitrust_isdigit(c: u8) -> bool {
// CHECK-NEXT:    c.is_ascii_digit()
// CHECK: fn __emitrust_isxdigit(c: u8) -> bool {
// CHECK-NEXT:    c.is_ascii_hexdigit()
// CHECK: fn __emitrust_isspace(c: u8) -> bool {
// CHECK-NEXT:    matches!(c, b'\t'..=b'\r' | b' ')
// CHECK: fn __emitrust_isprint(c: u8) -> bool {
// CHECK-NEXT:    matches!(c, b' '..=b'~')
// CHECK: fn __emitrust_isgraph(c: u8) -> bool {
// CHECK-NEXT:    c.is_ascii_graphic()
