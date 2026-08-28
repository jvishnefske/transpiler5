// FR-137 crash oracle, literal-backed shape: the reduced antirez/sds
// repro. Records NO whole-program base fact, but must still not abort.
//
// This is the shape found in the wild (sds.h:37 `extern const char
// *SDS_NOINIT;` + sds.c:42 `const char *SDS_NOINIT = "SDS_NOINIT";`), which
// crashed the importer outright -- see globals-pointer-extern-redecl.c for
// the root cause. It is the complement of that test: a string-literal
// binding is NOT the CTS-P4 real-object base shape, so
// `recordPointerGlobalFileScopeDetail` legitimately records nothing for it.
// The pin here is that "records nothing" is reached by RETURNING, never by
// dereferencing the absent decl-local initializer of the `extern`
// declaration.
//
// NOTE: emitrust-cc, not emitrust-import-c -- the whole-program pre-scan
// only runs on the project import path.
//
// RUN: emitrust-cc --emit=import %s | FileCheck %s

extern const char *SDS_NOINIT;

const char *SDS_NOINIT = "SDS_NOINIT";

int use(void) { return SDS_NOINIT[0]; }
int main(void) { return use(); }

// The literal gets its own const backing array and `SDS_NOINIT` becomes an
// i64 cursor into it -- exactly what the same program without the `extern`
// declaration emits.
// CHECK: emitrust.global const @SDS_NOINIT_BACKING <[83 : i8, 68 : i8, 83 : i8, 95 : i8, 78 : i8, 79 : i8, 73 : i8, 78 : i8, 73 : i8, 84 : i8, 0 : i8]> : !emitrust.array<11xi8>
// CHECK: emitrust.global @SDS_NOINIT <0 : i64> : i64
// CHECK-LABEL: func.func @use_
// CHECK: emitrust.global_load @SDS_NOINIT : i64
// CHECK: emitrust.global_load @SDS_NOINIT_BACKING : !emitrust.array<11xi8>
// CHECK: emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<11xi8>>, i64) -> !emitrust.lvalue<i8>
