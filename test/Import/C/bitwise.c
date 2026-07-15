// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | emitrust-opt --mem2reg --canonicalize --lift-cf-to-scf --canonicalize | FileCheck %s --check-prefix=SCF

int operators(int a, int b) {
  // Signed (signless) bitwise operators map directly onto arith ops.
  int r = a & b;
  r = r | (a ^ b);
  r = r & ~a;
  return r;
}

// CHECK-LABEL: func.func @operators
// CHECK: arith.andi
// CHECK: arith.xori
// CHECK: arith.ori
// `~a` is a XOR all-ones.
// CHECK: arith.constant -1 : i32
// CHECK: arith.xori
// CHECK: arith.andi
// SCF-LABEL: func.func @operators

int shifts(int v, long w, int n) {
  // C's `>>` on signed operands is an arithmetic shift on every relevant
  // ABI (and in Rust), hence shrsi. A shift amount keeps its own C type,
  // so `long << int` needs the amount widened to the shifted operand.
  int a = v << 2;
  int b = v >> n;
  long c = w << n;
  long d = w >> 3;
  return a + b + (int)(c + d);
}

// CHECK-LABEL: func.func @shifts
// CHECK: arith.shli %{{.*}}, %{{.*}} : i32
// CHECK: arith.shrsi %{{.*}}, %{{.*}} : i32
// CHECK: arith.extsi %{{.*}} : i32 to i64
// CHECK: arith.shli %{{.*}}, %{{.*}} : i64
// CHECK: arith.shrsi %{{.*}}, %{{.*}} : i64
// SCF-LABEL: func.func @shifts

int compound(int a, long w, int n) {
  // Bitwise and shift compound assignments, including `<<=` whose right
  // operand has a narrower type than the shifted long.
  a &= 0x0F;
  a |= 0x30;
  a ^= 5;
  a <<= 1;
  a >>= n;
  w <<= n;
  return a + (int)w;
}

// CHECK-LABEL: func.func @compound
// CHECK: arith.andi
// CHECK: arith.ori
// CHECK: arith.xori
// CHECK: arith.shli %{{.*}}, %{{.*}} : i32
// CHECK: arith.shrsi %{{.*}}, %{{.*}} : i32
// CHECK: arith.extsi %{{.*}} : i32 to i64
// CHECK: arith.shli %{{.*}}, %{{.*}} : i64
// SCF-LABEL: func.func @compound

int narrow(char c, short s) {
  // Narrow operands promote to int (clang's casts); the operators
  // themselves always see matching widths.
  return (c & s) | (c << 1);
}

// CHECK-LABEL: func.func @narrow
// CHECK: arith.extsi %{{.*}} : i8 to i32
// CHECK: arith.extsi %{{.*}} : i16 to i32
// CHECK: arith.andi %{{.*}}, %{{.*}} : i32
// CHECK: arith.shli %{{.*}}, %{{.*}} : i32
// CHECK: arith.ori %{{.*}}, %{{.*}} : i32
