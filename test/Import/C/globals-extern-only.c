// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// C99-14: an extern declaration with no definition in this translation
// unit has no storage to emit (multi-TU linking is out of scope).
extern int e;

int main(void) {
  return e;
}

// CHECK: globals-extern-only.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: extern global variable without a definition in this translation unit
