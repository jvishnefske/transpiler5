// RUN: split-file %s %t
// RUN: emitrust-import-c %t/block-scope.c | FileCheck %s --check-prefix=HOIST
// RUN: not emitrust-import-c %t/static-assert.c 2>&1 | FileCheck %s --check-prefix=REJECT

// CTS-S3: a block-scope function prototype has external linkage
// (C11 6.2.2p5), so it is hoisted to module scope and imported through
// the same path as a file-scope prototype: the declared symbol and the
// call come out identical to the file-scope spelling. Other unsupported
// block-scope declarations keep the located rejection.

//--- block-scope.c
int f2(char *);

int main(void) {
  int f1(char *);
  char s = 1;
  if (f1(&s) != f2(&s)) {
    return 1;
  }
  return 0;
}

int f1(char *p) {
  return *p + 1;
}

int f2(char *p) {
  return *p + 1;
}
// The block-scope prototype's symbol, call, and definition import exactly
// like the file-scope prototype's.
// HOIST: func.func @c_main
// HOIST: call @f1(%{{.*}}) : (!emitrust.mut_ref<i8>) -> i32
// HOIST: call @f2(%{{.*}}) : (!emitrust.mut_ref<i8>) -> i32
// HOIST: func.func @f1(%{{.*}}: !emitrust.mut_ref<i8>) -> i32
// HOIST: func.func @f2(%{{.*}}: !emitrust.mut_ref<i8>) -> i32

//--- static-assert.c
int main(void) {
  _Static_assert(1, "held");
  return 0;
}
// REJECT: static-assert.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported declaration inside a function body
