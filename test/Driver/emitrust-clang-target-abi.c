// FR-56: target/ABI flags that reach the -cc1 job affect type layout, so
// the shim must either FORWARD them into the import (the imported IR then
// reflects the layout the real compile used) or REJECT the translation
// unit into the artifact's ledger, located -- never silently import with
// the wrong layout. This test pins both directions:
//  - `-fshort-enums` is forwarded: the artifact's `sizeof(enum)` fold is 1
//    where the flagless artifact folds 4, and the SOLO import matches the
//    JOINT import given the same flag (the same FileCheck prefix runs over
//    both outputs);
//  - `-m32` is forwarded through the cc1 `-triple`: `long` becomes 4 bytes
//    wide and the C `unsigned long` maps to ui32;
//  - `-fpack-struct=` is in the measured cannot-honor set (the importer
//    models no struct packing and the emitted Rust has no repr story): the
//    real compile still succeeds and its object is kept, but the artifact
//    carries an EMPTY module whose ledger records the whole-TU rejection,
//    so the link step surfaces the reason while any symbol another TU
//    needs from this one fails loudly at merge instead of resolving
//    against wrong-layout IR.
//
// Baseline artifact: enum folds to 4 bytes, long to 8.
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.plain.o
// RUN: emitrust-opt %t.plain.o.emitrust.mlirbc -o - | FileCheck %s --check-prefix=PLAIN
// PLAIN: emitrust.func @enum_size
// PLAIN: emitrust.constant <4 : ui64>
// PLAIN: emitrust.func @long_size
// PLAIN: emitrust.constant <8 : ui64>
//
// -fshort-enums reaches the import: the enum now folds to 1 byte.
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -fshort-enums -c %s -o %t.short.o
// RUN: emitrust-opt %t.short.o.emitrust.mlirbc -o - | FileCheck %s --check-prefix=SHORT
// SHORT: emitrust.func @enum_size
// SHORT: emitrust.constant <1 : ui64>
//
// ... and the solo import matches the joint import given the same flag.
// RUN: emitrust-cc --emit=mlir --extra-arg=-fshort-enums %s -o - | FileCheck %s --check-prefix=SHORT
//
// -m32 reaches the import through the cc1 -triple: long is 4 bytes and
// unsigned long maps to ui32.
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -m32 -c %s -o %t.m32.o
// RUN: emitrust-opt %t.m32.o.emitrust.mlirbc -o - | FileCheck %s --check-prefix=M32
// M32: emitrust.func @long_size
// M32: emitrust.constant <4 : ui32>
//
// -fpack-struct cannot be honored: the build is untouched (exit 0, object
// kept), the shim warns, and the artifact is an empty module carrying the
// located whole-TU rejection in its ledger.
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -fpack-struct=2 -c %s -o %t.packed.o 2>%t.err
// RUN: FileCheck %s --check-prefix=PACKWARN < %t.err
// PACKWARN: warning: unsupported target/ABI flag '-fpack-struct=2'
// RUN: ls %t.packed.o
// RUN: emitrust-opt %t.packed.o.emitrust.mlirbc -o - | FileCheck %s --check-prefix=PACKED
// PACKED: module attributes
// PACKED-SAME: emitrust.rejections
// PACKED-SAME: unsupported target/ABI flag '-fpack-struct=2'
// PACKED-SAME: stubbed = false
// PACKED-SAME: symbol = "<translation unit>"
// PACKED-NOT: emitrust.func

enum Mode { MODE_A = 1, MODE_B = 2 };

unsigned long enum_size(void) { return sizeof(enum Mode); }

unsigned long long_size(void) { return sizeof(long); }
