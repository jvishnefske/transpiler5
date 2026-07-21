// C99-38/W3.0: a va_list-using variadic DEFINED in one translation unit
// and CALLED from another is a located rejection. planVaMonomorph
// enumerates a va_list variadic's call sites WITHIN A SINGLE TU only, so
// this file's call site never gets a specialized clone, and the
// definition's own TU never emits an ordinary symbol for it either (a
// va_list-monomorphized definition is always replaced by its per-site
// clones, never kept as a plain function) — without a located rejection
// this would be an unresolved cross-TU call escaping with no diagnostic.
// Real cross-TU monomorphization (W3.5) is DEFERRED — a synthesized clone
// has no C prototype, so a caller TU imported before its definer (the order
// the differential requires) has nothing to forward-declare it from, and the
// only fixes perturb multi-TU emission order or risk the 220/220 C ledger;
// see design.md's W3.0/W3.5 note for the full rationale. This rejection
// stands as the sound behavior.
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-varargs-def.c 2>&1 | FileCheck %s
// RUN: not emitrust-import-c %S/Inputs/multi-tu-varargs-def.c %s 2>&1 | FileCheck %s

// Negative control (must stay green): the SAME variadic defined AND
// called within ONE translation unit still monomorphizes fine — this
// wave's cross-TU rejection must not over-reject that case, which
// planVaMonomorph already handles (test/Import/C/varargs-monomorph.c).
// RUN: emitrust-import-c %S/Inputs/multi-tu-varargs-def.c | FileCheck %s --check-prefix=SAMETU

int logf_(const char *fmt, ...);

int main(void) { return logf_("%d", 5); }

// CHECK: multi-tu-varargs.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to a variadic function 'logf_' defined in another translation unit

// SAMETU: func.func {{.*}}logf_{{.*}}
