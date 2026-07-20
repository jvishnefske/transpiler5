// W3.1 multi-TU gate oracle — predicted failure #1 (shared `T *g;` header
// pointer global), and a LOUD finding for W3.2: relaxing G8's own
// soleTranslationUnit check in importPointerGlobal is NOT SUFFICIENT to
// make this realistic shape work.
//
// The most natural "shared pointer global" C shape is a header declaring
// `extern int *g;`, with exactly ONE translation unit providing the real
// definition. That is exactly what this file (the extern-only consumer)
// and its companion (the definition) do. But the rejection this pair
// hits TODAY is NOT G8's ImportCGlobals.cpp:102 check (wording "...with
// external linkage in a multi-file project") — it is a SEPARATE,
// UNCONDITIONAL restriction in `deferExternGlobal`
// (ImportCGlobals.cpp:423-425): ANY pointer-typed extern global that
// lacks a definition IN ITS OWN TU is hard-rejected before
// `soleTranslationUnit` is ever consulted, because `deferExternGlobal`
// is only reached for a referenced-but-undefined-here global in a
// project import (`deferExternGlobals` is true), and its very first
// check refuses every non-function-pointer type unconditionally.
//
// W3.2 MUST widen its relaxation to also cover this shorter-worded,
// unconditional gate — deferring a pointer-typed extern to
// `finalizeProject` and re-classifying it there once the defining TU's
// facts are known — or the headline "shared header pointer global"
// scenario stays broken even after G8 itself is fully relaxed. This test
// pins TODAY's rejection (order-independent: both processing orders hit
// the SAME wording, at the extern-only file's declaration line, because
// each TU is imported independently and `g`'s own TU never sees a local
// definition to satisfy `deferExternGlobal`).
//
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-gate-g8-ptr-global-shared-header-other.c 2>&1 | FileCheck %s
// RUN: not emitrust-import-c %S/Inputs/multi-tu-gate-g8-ptr-global-shared-header-other.c %s 2>&1 | FileCheck %s

extern int *g;

int read_g(void) { return *g; }
int main(void) { return read_g(); }

// Note the wording: shorter than G8's ("...with external linkage in a
// multi-file project") because this is `deferExternGlobal`'s
// unconditional check, not `importPointerGlobal`'s soleTU-gated one.
// CHECK: multi-tu-gate-g8-ptr-global-shared-header.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-typed global variable{{$}}
