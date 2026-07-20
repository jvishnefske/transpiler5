// W3.1 multi-TU gate oracle (G8): importPointerGlobal (CTS-P4,
// ImportCGlobals.cpp:102) hard-rejects any REFERENCED externally visible
// pointer-typed global in a multi-file project — planOwners' pointer-
// region facts (`globalPtrFacts`) are merged per TU, so another TU could
// rebind it behind this TU's already-consumed facts. This is the direct
// mechanism behind design.md CTS-P4's documented "external linkage in a
// multi-TU project" rejection.
//
// This test isolates the gate that fires when THIS TU holds the real
// definition and is itself referenced only from within this TU's own
// `read_g`/`main` — see multi-tu-gate-g8-ptr-global-shared-header.c for
// the (distinct, LOUDER) finding about what happens when the companion
// TU merely forward-declares `g` with `extern`.
//
// W3.2 will flip the `not` RUN line below to `emitrust-import-c ... |
// FileCheck` once `globalPtrFacts` is merged across every AST in the
// project before any TU's pointer globals are classified.
//
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-gate-g8-ptr-global-external-other.c 2>&1 | FileCheck %s

int arr[4];
int *g = &arr[0];
int read_g(void) { return *g; }
int main(void) { return read_g(); }

// CHECK: multi-tu-gate-g8-ptr-global-external.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-typed global variable with external linkage in a multi-file project
