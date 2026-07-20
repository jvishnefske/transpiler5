// W3.1 EXTERN-ARRAY COMPOSITE-MERGE PROBE — a LOUD finding for W3.2.
//
// C99 6.2.7 lets an incomplete-size `extern int a[];` in one translation
// unit be completed by the array's real definition (`int a[4] = ...;`)
// in another: the linker/compiler produces one COMPOSITE type. This is
// NOT a pointer-parameter shape at all, so it is untouched by the eight
// G1-G8 gates surveyed above — but the importer has NO cross-TU type
// composition step whatsoever: each translation unit is parsed as an
// independent `clang::ASTUnit` (see importCProject's per-file
// `ClangTool`), and `createGlobal`/`deferExternGlobal` map a global's
// type from THIS TU's OWN most-recent declaration
// (ImportCGlobals.cpp:71-77's "most recent decl ... merged composite of
// all redeclarations" comment is true only WITHIN one ASTContext — it
// cannot see another TU's redeclaration at all).
//
// Result (verified verbatim against the build in this wave): a TU whose
// ONLY declaration of `a` is the incomplete `extern int a[];` maps that
// declaration's own (incomplete) array type when `a` is used, and hits
// mapType's "non-constant array size" rejection at the EXTERN
// DECLARATION ITSELF — REGARDLESS of which TU is processed first, and
// regardless of whether the complete definition exists in the other TU
// at all. This is a REAL GAP, not merely a conservative gate: the
// BOUNDED baseline below shows the ordinary case (extern with an
// explicit, matching bound, `extern int a[4];`) already works fine
// today; only the incomplete-bound spelling is broken.
//
// W3.2 must give the importer some cross-TU composite-type step (at
// minimum: when a global is deferred as extern with an incomplete array
// type, defer its OWN type mapping to `finalizeProject` too, the same
// way its existence check is already deferred) before this probe can
// pass as a genuine differential.
//
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/incomplete-use.c %t/def.c 2>&1 | FileCheck %s --check-prefix=INCOMPLETE
// RUN: not emitrust-import-c %t/def.c %t/incomplete-use.c 2>&1 | FileCheck %s --check-prefix=INCOMPLETE
// RUN: emitrust-import-c %t/bounded-use.c %t/def.c | FileCheck %s --check-prefix=BOUNDED

//--- incomplete-use.c
// The incomplete-bound extern: C legally defers `a`'s size to the other
// TU's definition. The importer cannot see across TUs and maps THIS
// declaration's own (incomplete) type — a hard, verbatim-located
// rejection, independent of processing order.
extern int a[];
int read_a(int i) { return a[i]; }
int main(void) { return read_a(2); }

// INCOMPLETE: incomplete-use.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-constant array size

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
