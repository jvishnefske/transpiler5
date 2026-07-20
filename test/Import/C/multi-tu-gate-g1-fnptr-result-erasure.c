// W3.1 multi-TU gate oracle (G1): classifyFnPtrPointerResult (CTS-S/CTS-P2)
// erases a data-pointer fn-ptr RESULT type only when the candidate set of
// address-taken functions with a matching return type is a WHOLE-PROGRAM
// fact (importC.cpp classifyFnPtrPointerResult, ImportCTypes.cpp:679). In a
// multi-file project (importCProject, soleTranslationUnit = numASTs==1) the
// candidate set built from THIS TU's `addressTakenFunctions` could miss a
// function whose address is taken only in another TU, so the gate hard-
// rejects UNCONDITIONALLY rather than risk an unsound erasure — this is a
// sound-but-conservative rejection, not a real ambiguity (see the SAMETU
// baseline below, which proves the identical shape erases cleanly when this
// TU is the whole program).
//
// W3.2 (the whole-program address-taken hoist) will flip the two `not`
// RUN lines below to `emitrust-import-c ... | FileCheck` once
// `addressTakenFunctions` is collected across every AST before any TU is
// classified (mirroring collectCrossTuVaListVariadics's W3.0 pre-pass).
//
// RUN: emitrust-import-c %s | FileCheck %s --check-prefix=SAMETU
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-gate-g1-fnptr-result-erasure-other.c 2>&1 | FileCheck %s
// RUN: not emitrust-import-c %S/Inputs/multi-tu-gate-g1-fnptr-result-erasure-other.c %s 2>&1 | FileCheck %s

struct S {
  int m;
};
struct S gs;

struct S *go(void) { return &gs; }

// A reassigned (non-const) global fn-ptr whose signature returns a data
// pointer: `swap` below keeps it out of the CTS-S devirtualization alias
// (planFnPtrAliases only aliases a NEVER-reassigned target), so mapping
// this declaration's type genuinely reaches classifyFnPtrPointerResult.
struct S *(*p)(void) = &go;
void swap(void) { p = &go; }

int main(void) { return p()->m; }

// CHECK: multi-tu-gate-g1-fnptr-result-erasure.c:{{[0-9]+}}:13: error: unsupported: function pointer result type

// SAMETU baseline (sole TU, no companion): the identical shape erases the
// fn-ptr's result (no `-> ...` on the fn_ptr type) and routes `p()->m`
// through the ordinary staged-copy global machinery — no runtime pointer
// state, no rejection. This is the behavior W3.2 must reproduce for the
// multi-TU case above.
// SAMETU: emitrust.global @p <#emitrust.opaque<"Some(go)">> : !emitrust.fn_ptr<()>
// SAMETU-NOT: !emitrust.fn_ptr<() -> {{.*}}>
