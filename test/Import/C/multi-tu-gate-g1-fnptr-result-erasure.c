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
// W3.4 G1 flips this: the whole-program candidate-completeness fact
// (`dataPtrReturnFnAddressTakenTus`, built by collectWholeProgramInfo before
// any TU is classified) proves the sole candidate `go` returning `struct S *`
// is address-taken in ONE TU only, so this TU's per-TU `addressTakenFunctions`
// is the complete whole-program candidate set and the classifier runs — the
// multi-TU import now erases identically to the sole-TU baseline, in either
// TU order. (The divergent two-base shape stays rejected: see
// multi-tu-gate-g1-fnptr-result-diverge.c.)
//
// RUN: emitrust-import-c %s | FileCheck %s --check-prefix=SAMETU
// RUN: emitrust-import-c %s %S/Inputs/multi-tu-gate-g1-fnptr-result-erasure-other.c | FileCheck %s --check-prefix=SAMETU
// RUN: emitrust-import-c %S/Inputs/multi-tu-gate-g1-fnptr-result-erasure-other.c %s | FileCheck %s --check-prefix=SAMETU

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

// The fn-ptr result is erased (no `-> ...` on the fn_ptr type) and `p()->m`
// routes through the ordinary staged-copy global machinery — no runtime
// pointer state, no rejection. This SAMETU shape is asserted for the sole-TU
// baseline AND both multi-TU orderings above.
// SAMETU: emitrust.global @p <#emitrust.opaque<"Some(go)">> : !emitrust.fn_ptr<()>
// SAMETU-NOT: !emitrust.fn_ptr<() -> {{.*}}>
