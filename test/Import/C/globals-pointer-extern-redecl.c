// FR-137 crash oracle: a pointer global that is BOTH declared `extern` and
// defined in the same translation unit must import, not abort.
//
// The whole-program pre-scan (`collectWholeProgramInfo`,
// lib/ImportC/ImportC.cpp) walks EVERY decl of the TU, so a redeclaration
// chain of `int *g;` is visited once per declaration. Its pointer-global
// filter used the redecl-chain-wide `VarDecl::getAnyInitializer()` to decide
// there IS an initializer, but then handed the VISITED decl to
// `recordPointerGlobalFileScopeDetail`, whose `VarDecl::evaluateValue()` is
// DECL-LOCAL (clang's `evaluateValueImpl` reads `getInit()`). On the bare
// `extern int *g;` declaration that is null and clang dereferenced null
// inside `Expr::EvaluateAsInitializer` -- a SEGFAULT with no diagnostic and
// no recovery. `extern T *x;` in a header plus `T *x = ...;` in the .c is the
// ordinary C idiom, so this hit real code (antirez/sds sds.h:37 +
// sds.c:42, the shape pinned by globals-pointer-extern-redecl-literal.c).
//
// Removing the abort exposed a SECOND defect on the same shape:
// `importGlobalVar`'s redeclaration guard tested only `globals`, while a
// pointer global registers in `pointerGlobals` (the CTS-P4 cursor/backing
// decomposition), so the entity was imported once per declaration and the
// second pass died on "global variable 'G' collides with an existing
// symbol". Both halves are needed for this file to pass.
//
// This is the fact-CARRYING half: `&arr[1]` is the CTS-P4 shape 3
// single-base cursor binding, so the fix must keep RECORDING it, not merely
// skip the extern declaration. The CHECK lines below are byte-for-byte the
// module this program emits with the two `extern` lines DELETED -- the
// redeclaration must be transparent to the recorded fact. The reverse decl
// order is pinned by globals-pointer-extern-redecl-reversed.c.
//
// NOTE: this uses emitrust-cc rather than emitrust-import-c because the
// whole-program pre-scan only runs on the project import path, which
// emitrust-import-c reaches only with >=2 inputs.
//
// RUN: emitrust-cc --emit=import %s | FileCheck %s

extern int arr[4];
extern int *g;

int arr[4];
int *g = &arr[1];

int read_g(void) { return *g; }
int main(void) { return read_g(); }

// `g` is the CTS-P4 single-base cursor global: an i64 offset (element 1)
// against the base object `arr`; `*g` stages `arr` and subscripts it at the
// cursor. The cursor start of 1 is the fact that would be LOST if the fix
// merely skipped the extern declaration.
// CHECK: emitrust.global @ARR : !emitrust.array<4xi32>
// CHECK: emitrust.global @G <1 : i64> : i64
// CHECK-LABEL: func.func @read_g
// CHECK: emitrust.global_load @G : i64
// CHECK: emitrust.global_load @ARR : !emitrust.array<4xi32>
// CHECK: emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>
