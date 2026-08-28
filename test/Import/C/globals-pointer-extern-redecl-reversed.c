// FR-137 crash oracle, reverse declaration order: pins that decl ORDER is
// not what saves us.
//
// Companion to globals-pointer-extern-redecl.c (which carries the full
// root-cause note). There the `extern int *g;` declaration precedes the
// definition; here it FOLLOWS it. The pre-scan walks the TU's decls in
// source order and hands each one to `recordPointerGlobalFileScopeDetail`,
// so the trailing `extern` declaration -- still initializer-less, still
// decl-local-null -- reached the same `VarDecl::evaluateValue()` null
// dereference. Both orders must import, and both must record the SAME
// single-base cursor fact as the plain definition alone.
//
// NOTE: emitrust-cc, not emitrust-import-c -- the whole-program pre-scan
// only runs on the project import path.
//
// RUN: emitrust-cc --emit=import %s | FileCheck %s

int arr[4];
int *g = &arr[1];

extern int *g;

int read_g(void) { return *g; }
int main(void) { return read_g(); }

// Identical emitted module to the extern-first spelling: same base array,
// same cursor start.
// CHECK: emitrust.global @ARR : !emitrust.array<4xi32>
// CHECK: emitrust.global @G <1 : i64> : i64
// CHECK-LABEL: func.func @read_g
// CHECK: emitrust.global_load @G : i64
// CHECK: emitrust.global_load @ARR : !emitrust.array<4xi32>
// CHECK: emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>
