// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// C99-14 boundary: _Thread_local storage is outside the imported subset.
_Thread_local int t;

int main(void) {
  return t;
}

// CHECK: globals-thread-local.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: thread-local global variable
