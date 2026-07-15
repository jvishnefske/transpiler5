// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// C99-14: global names keep their C spelling verbatim, so a name that is a
// Rust keyword is rejected rather than silently mangled.
int loop = 1;

int main(void) {
  return loop;
}

// CHECK: globals-keyword.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global variable name 'loop' is a Rust keyword
