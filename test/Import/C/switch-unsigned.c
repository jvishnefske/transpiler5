// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | emitrust-opt --mem2reg --canonicalize --lift-cf-to-scf --canonicalize | FileCheck %s --check-prefix=SCF

int on_uint(unsigned int u) {
  // An unsigned scrutinee is reinterpreted to signless i64 with
  // emitrust.cast (zero-extending for ui32); case labels zero-extend their
  // bits identically, so values above INT32_MAX still match.
  switch (u) {
  case 1u:
    return 1;
  case 4000000000u:
    return 2;
  default:
    return 0;
  }
}

// CHECK-LABEL: func.func @on_uint
// CHECK: emitrust.cast %{{.*}} : ui32 to i64
// CHECK: cf.switch %{{[0-9]+}} : i64, [
// CHECK-NEXT: default: ^bb{{[0-9]+}},
// CHECK-NEXT: 1: ^bb{{[0-9]+}},
// CHECK-NEXT: 4000000000: ^bb{{[0-9]+}}
// CHECK-NEXT: ]
// SCF-LABEL: func.func @on_uint

int on_ulonglong(unsigned long long v) {
  // ui64 values reinterpret their bit pattern into i64; in-range case
  // labels match unchanged (labels above i64::MAX live in
  // switch-unsigned64.c, which documents a cf.switch textual round-trip
  // caveat).
  switch (v) {
  case 5ull:
    return 2;
  default:
    return 0;
  }
}

// CHECK-LABEL: func.func @on_ulonglong
// CHECK: emitrust.cast %{{.*}} : ui64 to i64
// CHECK: cf.switch
// SCF-LABEL: func.func @on_ulonglong

int narrow_promotes(unsigned char c) {
  // unsigned char promotes to plain int in the switch condition, so the
  // scrutinee is already signless and needs no reinterpretation.
  switch (c) {
  case 7:
    return 1;
  default:
    return 0;
  }
}

// CHECK-LABEL: func.func @narrow_promotes
// CHECK: emitrust.cast %{{.*}} : ui8 to i32
// CHECK: cf.switch
