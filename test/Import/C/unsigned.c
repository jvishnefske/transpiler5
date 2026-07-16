// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | emitrust-opt --mem2reg --canonicalize --lift-cf-to-scf --canonicalize | FileCheck %s --check-prefix=SCF

unsigned int arithmetic(unsigned int a, unsigned int b) {
  // Unsigned scalars live in emitrust.variable places (mem2reg would
  // materialize a signless default for a memref cell) and all arithmetic
  // uses emitrust ops (arith requires signless operands).
  unsigned int c = a + b;
  c = c - a;
  c = c * 2u;
  c = c / 3u;
  return c % b;
}

// CHECK-LABEL: func.func @arithmetic
// CHECK-SAME: (%{{.*}}: ui32, %{{.*}}: ui32) -> ui32
// CHECK: emitrust.variable : !emitrust.lvalue<ui32>
// CHECK: emitrust.add %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.sub %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.constant <2 : ui32> : ui32
// CHECK: emitrust.mul %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.div %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.rem %{{.*}}, %{{.*}} : ui32
// CHECK-NOT: arith.addi
// SCF-LABEL: func.func @arithmetic

int comparisons(unsigned int a, unsigned int b) {
  // Unsigned comparisons are emitrust.cmp: the Rust infix operators are
  // type-directed and hence unsigned, matching C.
  int r = 0;
  if (a < b) r += 1;
  if (a <= b) r += 2;
  if (a > b) r += 4;
  if (a >= b) r += 8;
  if (a == b) r += 16;
  if (a != b) r += 32;
  return r;
}

// CHECK-LABEL: func.func @comparisons
// CHECK: emitrust.cmp lt, %{{.*}}, %{{.*}} : (ui32, ui32) -> i1
// CHECK: emitrust.cmp le, %{{.*}}, %{{.*}} : (ui32, ui32) -> i1
// CHECK: emitrust.cmp gt, %{{.*}}, %{{.*}} : (ui32, ui32) -> i1
// CHECK: emitrust.cmp ge, %{{.*}}, %{{.*}} : (ui32, ui32) -> i1
// CHECK: emitrust.cmp eq, %{{.*}}, %{{.*}} : (ui32, ui32) -> i1
// CHECK: emitrust.cmp ne, %{{.*}}, %{{.*}} : (ui32, ui32) -> i1
// SCF-LABEL: func.func @comparisons

int truthiness(unsigned long u) {
  // An unsigned condition compares against an unsigned zero constant.
  if (u) {
    return 1;
  }
  while (u) {
    return 2;
  }
  return 0;
}

// CHECK-LABEL: func.func @truthiness
// CHECK: emitrust.constant <0 : ui64> : ui64
// CHECK: emitrust.cmp ne, %{{.*}}, %{{.*}} : (ui64, ui64) -> i1

unsigned long widths(unsigned char c, unsigned short s, unsigned int i) {
  // Widening between unsigned ranks is emitrust.cast (Rust `as`
  // zero-extends from unsigned sources, exactly C's conversions).
  unsigned long l = c;
  l = l + s;
  l = l + i;
  return l;
}

// CHECK-LABEL: func.func @widths
// CHECK-SAME: (%{{.*}}: ui8, %{{.*}}: ui16, %{{.*}}: ui32) -> ui64
// CHECK: emitrust.cast %{{.*}} : ui8 to ui64
// CHECK: emitrust.cast %{{.*}} : ui16 to ui64
// CHECK: emitrust.cast %{{.*}} : ui32 to ui64

unsigned int cross_sign(int s, unsigned int u) {
  // Mixed signed/unsigned expressions arrive behind clang's usual
  // arithmetic conversion casts; sign-domain changes are emitrust.cast.
  unsigned int a = s;         // i32 -> ui32 (same width, reinterpret)
  int b = (int)u;             // ui32 -> i32 (explicit)
  long w = (long)u;           // ui32 -> i64 (zero-extends)
  return a + u + (unsigned int)(b + (int)w);
}

// CHECK-LABEL: func.func @cross_sign
// CHECK: emitrust.cast %{{.*}} : i32 to ui32
// CHECK: emitrust.cast %{{.*}} : ui32 to i32
// CHECK: emitrust.cast %{{.*}} : ui32 to i64

double to_float(unsigned int u) {
  // Unsigned to floating is emitrust.cast (`u32 as f64` matches C).
  return (double)u + u;
}

// CHECK-LABEL: func.func @to_float
// CHECK: emitrust.cast %{{.*}} : ui32 to f64

unsigned int from_float(double d) {
  // Floating to unsigned is emitrust.cast; in-range values convert like C
  // (out of range, C is undefined and Rust saturates).
  return (unsigned int)d;
}

// CHECK-LABEL: func.func @from_float
// CHECK: emitrust.cast %{{.*}} : f64 to ui32

unsigned int bitwise(unsigned int a, unsigned int b) {
  // Unsigned bitwise and shift operators lower to the emitrust bitwise
  // ops; Rust's `>>` on uN is logical, exactly C's unsigned shift.
  unsigned int r = a & b;
  r = r | (a ^ b);
  r = r << 2;
  r = r >> 3u;
  r &= ~b;
  return r;
}

// CHECK-LABEL: func.func @bitwise
// CHECK: emitrust.and %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.xor %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.or %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.cast %{{.*}} : i32 to ui32
// CHECK: emitrust.shl %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.shr %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.constant <4294967295 : ui32> : ui32
// CHECK: emitrust.xor %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.and %{{.*}}, %{{.*}} : ui32

unsigned int updates(unsigned int u) {
  // Compound assignment and ++/-- on unsigned operands.
  u += 5u;
  u -= 1u;
  u *= 2u;
  u++;
  --u;
  return u;
}

// CHECK-LABEL: func.func @updates
// CHECK: emitrust.add %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.sub %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.mul %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.constant <1 : ui32> : ui32
// CHECK: emitrust.add %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.sub %{{.*}}, %{{.*}} : ui32

unsigned int literals(void) {
  // Unsigned literal forms become emitrust.constant of the unsigned type.
  unsigned int a = 42u;
  unsigned int b = 0xFFu;
  unsigned long c = 4294967296ul;
  return a + b + (unsigned int)c;
}

// CHECK-LABEL: func.func @literals
// CHECK: emitrust.constant <42 : ui32> : ui32
// CHECK: emitrust.constant <255 : ui32> : ui32
// CHECK: emitrust.constant <4294967296 : ui64> : ui64

unsigned int negate(unsigned int u) {
  // C negates an unsigned value modulo 2^N; unary '-' lowers to
  // `0 - x` through emitrust.sub, whose unsigned form renders as Rust's
  // wrapping_sub (the infix `-` would panic on debug overflow).
  return -u;
}

// CHECK-LABEL: func.func @negate
// CHECK: emitrust.constant <0 : ui32> : ui32
// CHECK: emitrust.sub %{{.*}}, %{{.*}} : ui32
// CHECK-NOT: arith.subi
// SCF-LABEL: func.func @negate

unsigned long negate_long(unsigned long u) {
  return -u;
}

// CHECK-LABEL: func.func @negate_long
// CHECK: emitrust.constant <0 : ui64> : ui64
// CHECK: emitrust.sub %{{.*}}, %{{.*}} : ui64
// SCF-LABEL: func.func @negate_long

unsigned int negate_expr(unsigned int a, unsigned int b) {
  // Negation composes with the other unsigned arithmetic ops.
  return -a + b * -2u;
}

// CHECK-LABEL: func.func @negate_expr
// CHECK: emitrust.constant <0 : ui32> : ui32
// CHECK: emitrust.sub %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.constant <2 : ui32> : ui32
// CHECK: emitrust.constant <0 : ui32> : ui32
// CHECK: emitrust.sub %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.mul %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.add %{{.*}}, %{{.*}} : ui32

int negate_compare(unsigned int a, unsigned int b) {
  // A negated unsigned operand keeps the unsigned comparison lowering.
  return -a < b;
}

// CHECK-LABEL: func.func @negate_compare
// CHECK: emitrust.constant <0 : ui32> : ui32
// CHECK: emitrust.sub %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.cmp lt, %{{.*}}, %{{.*}} : (ui32, ui32) -> i1

unsigned int negate_assign(unsigned int a) {
  // Negation in assignment position stores through the lvalue cell.
  unsigned int r;
  r = -a;
  r = -r;
  return r;
}

// CHECK-LABEL: func.func @negate_assign
// CHECK: emitrust.constant <0 : ui32> : ui32
// CHECK: emitrust.sub %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.assign
// CHECK: emitrust.constant <0 : ui32> : ui32
// CHECK: emitrust.sub %{{.*}}, %{{.*}} : ui32
// CHECK: emitrust.assign
