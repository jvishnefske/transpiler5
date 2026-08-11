// RUN: emitrust-import-c %s | FileCheck %s
// W2.12: a literal-initialized `std::string_view` LOCAL decomposes at
// import into (shared read-only literal backing byte array, i64 cursor
// cell, i64 len cell) — no string_view type is ever materialized, so the
// mapType tail's rejection stays authoritative for every other
// string_view position (stl-invalid.cpp pins that frontier). Pins the
// chosen lowerings at the IR level: the backing is the SAME const
// emitrust.variable a C `char *p = "lit"` region builds (bytes plus the
// terminating NUL), the cursor cell initializes to 0 and the len cell to
// the literal's length WITHOUT the NUL; size() loads the len cell and
// casts to the call's declared C type (the emitLenCall convention);
// remove_prefix(n) is the cursor += n / len -= n pair; sv[i] subscripts
// the backing at cursor + i and loads an i8 (C `char` semantics). The
// CHECK ordering also pins that a size() read BEFORE remove_prefix keeps
// its pre-mutation value: the `full` load of the len cell appears before
// the mutation's stores, and the post-mutation size() re-loads the cell.

extern "C" int printf(const char *, ...);

#include <string_view>

// CHECK-LABEL: func.func @view_ops
// The two i64 state cells are entry allocas (created cursor-then-len;
// entry-start insertion prints the later creation first).
// CHECK: %[[LEN:.*]] = memref.alloca() : memref<i64>
// CHECK: %[[CUR:.*]] = memref.alloca() : memref<i64>
// The shared backing: "cpp17-corpus" bytes plus the terminating NUL, in
// the same const emitrust.variable shape a C literal region builds.
// CHECK: %[[BACK:.*]] = emitrust.variable const <[99 : i8, 112 : i8, 112 : i8, 49 : i8, 55 : i8, 45 : i8, 99 : i8, 111 : i8, 114 : i8, 112 : i8, 117 : i8, 115 : i8, 0 : i8]> : !emitrust.lvalue<!emitrust.array<13xi8>>
// CHECK-NOT: emitrust.variable const
// Decl-site init: cursor 0, len 12 (the literal's length WITHOUT the NUL).
// CHECK: %[[ZERO:.*]] = arith.constant 0 : i64
// CHECK: memref.store %[[ZERO]], %[[CUR]][] : memref<i64>
// CHECK: %[[TWELVE:.*]] = arith.constant 12 : i64
// CHECK: memref.store %[[TWELVE]], %[[LEN]][] : memref<i64>
// `int full = sv.size();` BEFORE the mutation: len cell load, cast to the
// call's declared C type (size_t -> ui64) — pinned before the
// remove_prefix stores below, so the pre-mutation value is what `full`
// keeps.
// CHECK: %[[FULL:.*]] = memref.load %[[LEN]][] : memref<i64>
// CHECK: emitrust.cast %[[FULL]] : i64 to ui64
// remove_prefix(6): the argument (already size_t-converted by clang)
// casts to i64, then cursor += n; len -= n.
// CHECK: %[[AMT:.*]] = emitrust.cast %{{.*}} : ui64 to i64
// CHECK: %[[CUR0:.*]] = memref.load %[[CUR]][] : memref<i64>
// CHECK: %[[ADV:.*]] = arith.addi %[[CUR0]], %[[AMT]] : i64
// CHECK: memref.store %[[ADV]], %[[CUR]][] : memref<i64>
// CHECK: %[[LEN0:.*]] = memref.load %[[LEN]][] : memref<i64>
// CHECK: %[[TRIM:.*]] = arith.subi %[[LEN0]], %[[AMT]] : i64
// CHECK: memref.store %[[TRIM]], %[[LEN]][] : memref<i64>
// Post-mutation size() re-loads the len cell.
// CHECK: %[[REST:.*]] = memref.load %[[LEN]][] : memref<i64>
// CHECK: emitrust.cast %[[REST]] : i64 to ui64
// sv[0]: subscript the backing at cursor + i, load an i8 byte.
// CHECK: %[[CUR1:.*]] = memref.load %[[CUR]][] : memref<i64>
// CHECK: %[[POS:.*]] = arith.addi %[[CUR1]], %{{.*}} : i64
// CHECK: %[[BYTE:.*]] = emitrust.subscript %[[BACK]][%[[POS]]] : (!emitrust.lvalue<!emitrust.array<13xi8>>, i64) -> !emitrust.lvalue<i8>
// CHECK: emitrust.load %[[BYTE]] : (!emitrust.lvalue<i8>) -> i8
int view_ops(void) {
  std::string_view sv = "cpp17-corpus";
  int full = sv.size();
  sv.remove_prefix(6);
  int rest = sv.size();
  int c0 = sv[0];
  return full + rest + c0;
}
