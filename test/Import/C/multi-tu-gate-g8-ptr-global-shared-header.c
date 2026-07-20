// W3.1 multi-TU gate oracle — predicted failure #1 (shared `T *g;` header
// pointer global) — FLIPPED GREEN by W3.2 COMMIT B.
//
// The most natural "shared pointer global" C shape is a header declaring
// `extern int *g;`, with exactly ONE translation unit providing the real
// definition. That is exactly what this file (the extern-only consumer)
// and its companion (the definition) do. W3.1 found the rejection this
// pair hit was NOT G8's own `importPointerGlobal` soleTranslationUnit
// check (ImportCGlobals.cpp, "...with external linkage in a multi-file
// project") — it was a SEPARATE, UNCONDITIONAL restriction in
// `deferExternGlobal`: ANY pointer-typed extern global that lacks a
// definition IN ITS OWN TU was hard-rejected before `soleTranslationUnit`
// was ever consulted, because the companion TU's `g` is never itself
// REFERENCED there (only initialized), so `importPointerGlobal`'s
// referenced-only skip fires for it and G8 never even runs.
//
// W3.2 COMMIT B's fix: the whole-program pre-scan
// (`collectWholeProgramInfo`) evaluates `g`'s file-scope initializer in
// the DEFINING TU (`&arr[0]`, a sole, never-reassigned, real-object
// binding — the sound single-region shape) and records its base object
// (`arr`'s own canonical `Decl*`, valid for the whole `importCProject`
// call since every parsed AST stays alive) and flat cursor start.
// `deferExternGlobal` (which now delegates a pointer-typed extern to
// `deferExternPointerGlobal`) consults this fact: it EAGERLY re-imports
// `arr` from its own TU's `ASTContext` (idempotent regardless of which TU
// is processed first) and synthesizes `g`'s own `i64` cursor global here,
// since the defining TU never created one (its own `g` is never locally
// referenced). This test stays order-independent: both processing orders
// below produce a sound, verified module.
//
// RUN: emitrust-import-c %s %S/Inputs/multi-tu-gate-g8-ptr-global-shared-header-other.c | FileCheck %s
// RUN: emitrust-import-c %S/Inputs/multi-tu-gate-g8-ptr-global-shared-header-other.c %s | FileCheck %s

extern int *g;

int read_g(void) { return *g; }
int main(void) { return read_g(); }

// CHECK-DAG: emitrust.global @arr <[10 : i32, 20 : i32, 30 : i32, 40 : i32]> : !emitrust.array<4xi32>
// CHECK-DAG: emitrust.global @g <0 : i64> : i64
// CHECK-DAG: func.func @read_g() -> i32
// CHECK-DAG: func.func @c_main() -> i32
