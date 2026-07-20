// W3.1 multi-TU gate oracle (G7): planFnPtrAliases (CTS-S, 00189) only
// aliases a file-scope function pointer that is externally visible when
// this TU is the whole program (ImportC.cpp:2538) — another TU could
// reassign it behind this TU's facts. UNLIKE G1/G2/G8, this is a
// genuinely silent, correctness-preserving fallback: `addptr` still
// imports as an ordinary `!emitrust.fn_ptr` global, and calls through it
// still lower to `emitrust.call_indirect` — no error, no cell-slice-style
// "historical rejection" regression, purely the devirtualization
// optimization lost. This is the one gate of the G3/G4/G5/G6/G7 "soft"
// family that actually behaves as design.md's "conservative-but-sound,
// disables cross-TU OPTIMIZATIONS, not correctness" framing promises for
// EVERY case (contrast with G4/G5/G6, whose equivalent global-array-
// parameter shape currently regresses to a real compile error — see
// multi-tu-gate-g4-cellslice-poison.c).
//
// W3.2 will flip the CHECK lines below from `call_indirect` /
// `emitrust.global @addptr` to the devirtualized direct-call shape
// test/Import/C/fnptr-devirt.c already asserts for the sole-TU case,
// once fnPtrAliases is decided from a whole-program view of every TU's
// writes to `addptr` (mirroring fnPtrGlobalsWritten's per-TU scan today).
//
// RUN: emitrust-import-c %s %S/Inputs/multi-tu-gate-g7-fnptr-devirt-skip-other.c | FileCheck %s

int add(int a, int b) { return a + b; }

// A const-qualified, never-reassigned global fn-ptr: in a sole-TU import
// (test/Import/C/fnptr-devirt.c) this devirtualizes to a direct call. In
// a multi-file project it stays a real fn_ptr value because another TU
// could still observe or (if not `const`) rewrite it.
int (*const addptr)(int, int) = &add;

int direct_devirt(void) { return addptr(2, 3); }

int main(void) { return direct_devirt(); }

// CHECK: emitrust.global @addptr <#emitrust.opaque<"Some(add)">> : !emitrust.fn_ptr<(i32, i32) -> i32>
// CHECK-LABEL: func.func @direct_devirt
// CHECK: emitrust.global_load @addptr
// CHECK: emitrust.call_indirect %{{.*}}(%{{.*}}, %{{.*}}) : (!emitrust.fn_ptr<(i32, i32) -> i32>, i32, i32) -> i32
