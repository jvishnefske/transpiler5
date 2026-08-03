// FR-56: `-include <file>` (and `-idirafter`) are SEMANTIC preprocessor
// inputs — the same class as `-D` — and must reach the import. The Linux
// kernel is the motivating corpus: every kernel TU's semantics live behind
// `-include ./include/linux/compiler_types.h`, so a shim that drops the
// flag imports a different program than the one the real compile built
// (measured: mass parse failure instead of ledgered rejections).
//
// RUN: echo "#define INJ 9" > %t.inj.h
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -include %t.inj.h -c %s -o %t.o
// RUN: emitrust-opt %t.o.emitrust.mlirbc -o - | FileCheck %s
// CHECK: emitrust.func @injected
// CHECK: emitrust.constant <9 : i32>

int injected(void) { return INJ; }
