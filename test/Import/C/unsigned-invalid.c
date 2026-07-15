// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

unsigned int negate(unsigned int u) {
  // C negates unsigned values modulo 2^N, but the `0 - x` this would lower
  // to panics on overflow in debug Rust; rejected until a wrapping
  // negation lowering exists.
  return -u;
}

// CHECK: unsigned-invalid.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: unary '-' on an unsigned operand
