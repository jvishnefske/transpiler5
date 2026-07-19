// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | emitrust-opt --mem2reg --canonicalize --lift-cf-to-scf --canonicalize | FileCheck %s --check-prefix=SCF

// C99-4: plain-char signedness policy and character constants. Plain
// `char` maps to signless i8 exactly like `signed char` (the x86-64
// Linux / clang default the differential oracle uses); a character
// constant has C type int and imports as the i32 constant clang
// evaluated, including escape sequences and — under the signed-char
// policy — sign-extended high bytes ('\xff' is -1).

int letter(void) { return 'a'; }
// CHECK-LABEL: func.func @letter
// CHECK: arith.constant 97 : i32
// SCF-LABEL: func.func @letter

int escapes(void) {
  // Simple escapes, octal, and hex, all evaluated by clang to their
  // single-byte values and imported as plain i32 constants.
  int n = '\n';
  int t = '\t';
  int nul = '\0';
  int backslash = '\\';
  int quote = '\'';
  int dquote = '\"';
  int octal = '\012';
  int hex = '\x41';
  return n + t + nul + backslash + quote + dquote + octal + hex;
}
// CHECK-LABEL: func.func @escapes
// CHECK-DAG: arith.constant 10 : i32
// CHECK-DAG: arith.constant 9 : i32
// CHECK-DAG: arith.constant 0 : i32
// CHECK-DAG: arith.constant 92 : i32
// CHECK-DAG: arith.constant 39 : i32
// CHECK-DAG: arith.constant 34 : i32
// CHECK-DAG: arith.constant 65 : i32
// SCF-LABEL: func.func @escapes

int high_byte(void) {
  // Signed plain char: '\xff' is the int -1, not 255.
  return '\xff';
}
// CHECK-LABEL: func.func @high_byte
// CHECK: arith.constant -1 : i32
// SCF-LABEL: func.func @high_byte

int wide(void) {
  // A wide constant is an int-typed rvalue (wchar_t is int on this
  // target) and imports as its code-point value verbatim, matching the
  // wide-string policy; the NUL shape is pinned by c-testsuite 00098.
  return L'A' + L'\0';
}
// CHECK-LABEL: func.func @wide
// CHECK-DAG: arith.constant 65 : i32
// CHECK-DAG: arith.constant 0 : i32
// SCF-LABEL: func.func @wide

int narrow_store_and_compare(char c) {
  // A plain-char parameter is signless i8 (same mapping as signed
  // char). Initializing a char from a constant truncates the int-typed
  // constant; comparing promotes the char back to int (sign-extending).
  char z = 'Z';
  if (c == 'q')
    return z;
  return c < 'A';
}
// CHECK-LABEL: func.func @narrow_store_and_compare
// CHECK-SAME: (%{{.*}}: i8)
// CHECK: arith.constant 90 : i32
// CHECK: arith.trunci %{{.*}} : i32 to i8
// CHECK: arith.extsi %{{.*}} : i8 to i32
// CHECK: arith.constant 113 : i32
// CHECK: arith.cmpi eq
// CHECK: arith.cmpi slt
// SCF-LABEL: func.func @narrow_store_and_compare

int classify(char c) {
  // Character constants as switch case labels: the scrutinee promotes
  // to int and the labels carry the evaluated constant values.
  switch (c) {
  case 'a':
    return 1;
  case '\n':
    return 2;
  case '\x7f':
    return 3;
  default:
    return 0;
  }
}
// CHECK-LABEL: func.func @classify
// CHECK: cf.switch
// CHECK-DAG: 97: ^bb{{[0-9]+}}
// CHECK-DAG: 10: ^bb{{[0-9]+}}
// CHECK-DAG: 127: ^bb{{[0-9]+}}
// SCF-LABEL: func.func @classify

int subscript(void) {
  // Character constants in array subscripts: 'b' - 'a' folds through
  // ordinary int arithmetic to index 1.
  int table[3] = {5, 6, 7};
  return table['b' - 'a'];
}
// CHECK-LABEL: func.func @subscript
// CHECK-DAG: arith.constant 98 : i32
// CHECK-DAG: arith.constant 97 : i32
// CHECK: arith.subi
// CHECK: emitrust.subscript
// CHECK: emitrust.load
// SCF-LABEL: func.func @subscript
