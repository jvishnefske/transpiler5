// W3.1 EXTERN-ARRAY COMPOSITE-MERGE PROBE — FLIPPED GREEN by W3.2 COMMIT B.
//
// C99 6.2.7 lets an incomplete-size `extern int a[];` in one translation
// unit be completed by the array's real definition (`int a[4] = ...;`)
// in another: the linker/compiler produces one COMPOSITE type. This is
// NOT a pointer-parameter shape at all, so it is untouched by the eight
// G1-G8 gates surveyed above — but each translation unit parses as an
// independent `clang::ASTUnit` (see importCProject's per-file
// `ClangTool`), and W3.1 found the importer had NO cross-TU type
// composition step at all: a TU whose ONLY declaration of `a` is the
// incomplete `extern int a[];` mapped that declaration's own (incomplete)
// array type when `a` was used, hitting mapType's "non-constant array
// size" rejection at the extern declaration itself, independent of
// processing order or whether the completing definition existed at all.
//
// W3.2 COMMIT B's fix: the whole-program pre-scan
// (`collectWholeProgramInfo`) records, for every externally visible
// bounded-array DEFINITION in any TU, its complete element type (mapped
// with a small memoization-free scalar mapper deliberately narrower than
// the real `mapType`, so this speculative pre-scan never touches
// `mapType`'s Pass-A-dependent byte-region-aggregate caches before this
// TU's own Pass-A has run). `deferExternGlobal` resolves an
// incomplete-array extern's type from this whole-program fact instead of
// its own (incomplete) declaration when a completing definition exists
// project-wide -- mirroring how the EXISTENCE check is already deferred
// via `pendingExternGlobals`. The BOUNDED baseline below shows the
// already-supported case (an extern with an explicit, matching bound)
// is unaffected.
//
// RUN: split-file %s %t
// RUN: emitrust-import-c %t/incomplete-use.c %t/def.c | FileCheck %s --check-prefix=INCOMPLETE
// RUN: emitrust-import-c %t/def.c %t/incomplete-use.c | FileCheck %s --check-prefix=INCOMPLETE
// RUN: emitrust-import-c %t/bounded-use.c %t/def.c | FileCheck %s --check-prefix=BOUNDED

//--- incomplete-use.c
// The incomplete-bound extern: C legally defers `a`'s size to the other
// TU's definition. The whole-program pre-scan resolves the composite
// type from `def.c`'s complete definition regardless of processing order.
extern int a[];
int read_a(int i) { return a[i]; }
int main(void) { return read_a(2); }

// INCOMPLETE-DAG: emitrust.global @a <[10 : i32, 20 : i32, 30 : i32, 40 : i32]> : !emitrust.array<4xi32>
// INCOMPLETE-DAG: func.func @read_a(%{{.*}}: i32) -> i32
// INCOMPLETE-DAG: func.func @c_main() -> i32

//--- def.c
int a[4] = {10, 20, 30, 40};

//--- bounded-use.c
// BOUNDED baseline sanity: an extern with an EXPLICIT, matching bound is
// unaffected by this gap — the ordinary already-supported case, proving
// the probe's failure is specific to the incomplete-size spelling.
extern int a[4];
int read_a(int i) { return a[i]; }
int main(void) { return read_a(2); }

// BOUNDED-LABEL: func.func @read_a
// BOUNDED-LABEL: func.func @c_main
// BOUNDED: emitrust.global @a
