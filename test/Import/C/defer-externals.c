// FR-57a deferred-externals import mode: under --defer-externals a
// referenced-but-undefined external symbol is no longer a whole-program
// error. An undefined extern GLOBAL becomes a declaration-only
// `emitrust.global` (no initializer) carrying the `emitrust.extern_decl`
// unit attribute, and a referenced body-less FUNCTION keeps its declaration
// with the same marker — both are link-time obligations the FR-58 merge
// step must resolve, recorded in the per-TU shard instead of rejected at
// import time. A module still carrying the marker CANNOT become Rust: the
// emitter refuses it with a located error, so forgetting the link step can
// never silently emit a crate that reads a symbol nobody defines.
//
// The markers at import level (globals appended by finalizeProject, so they
// print after the functions):
// RUN: emitrust-cc --defer-externals --emit=import %s | FileCheck %s
//
// The body-less function and the declaration-only global both survive the
// conversion pipeline with the marker intact:
// RUN: emitrust-cc --defer-externals --emit=mlir %s | FileCheck %s --check-prefix=MLIR
//
// Direct crate emission of a deferred module is refused by the emitter:
// RUN: not emitrust-cc --defer-externals --emit=crate %s -o %t.crate 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT
//
// Regression pin: without the flag the historical whole-program rejection
// is byte-for-byte unchanged:
// RUN: not emitrust-cc --emit=mlir %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=NODEFER

extern int shared_counter;
extern int table[4];

int helper(int x);

int bump(int amount) {
  shared_counter = shared_counter + amount;
  return helper(table[1]) + shared_counter;
}

int local_double(int x) { return x + x; }

// The body-less function keeps its declaration, marked:
// CHECK-DAG: func.func private @helper(i32) -> i32 attributes {emitrust.extern_decl}
// The defined functions are present and reference the deferred symbols:
// CHECK-DAG: func.func @bump
// CHECK-DAG: call @helper(
// CHECK-DAG: func.func @local_double
// The deferred globals are declaration-only (no `<init>`), marked, and
// carry the FR-53 idiomatic global spelling:
// CHECK-DAG: emitrust.global @SHARED_COUNTER {emitrust.extern_decl} : i32
// CHECK-DAG: emitrust.global @TABLE {emitrust.extern_decl} : !emitrust.array<4xi32>

// MLIR-DAG: emitrust.func private @helper(i32) -> i32 attributes {emitrust.extern_decl}
// MLIR-DAG: emitrust.func @bump
// MLIR-DAG: emitrust.global @SHARED_COUNTER {emitrust.extern_decl} : i32
// MLIR-DAG: emitrust.global @TABLE {emitrust.extern_decl} : !emitrust.array<4xi32>

// The walk finds the body-less function first (the deferred globals are
// appended at module end), and the error is located at its declaration:
// STRICT: defer-externals.c:32:5: error: unresolved deferred external 'helper': the module must be linked against the defining translation unit before Rust emission

// NODEFER: error: unsupported: extern global variable '{{(SHARED_COUNTER|TABLE)}}' is referenced but not defined in any translation unit
