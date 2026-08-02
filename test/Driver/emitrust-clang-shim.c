// FR-56 compiler-shim spike: `emitrust-clang` behaves as a drop-in C
// compiler. A `-c` compile delegates to the real clang (the object it
// produces is byte-identical to a plain-clang compile of the same command
// line) and side-emits one `<object>.emitrust.mlir` artifact holding the
// imported, converted emitrust module -- with `-D` macros provably reaching
// the import. Non-compile invocations (`--version`, `-E`) pass through
// verbatim and emit no artifact. An import rejection must NOT fail the
// build: the volatile local below would reject a strict import, but the
// shim's recover-mode import still lets clang's exit code (0) through.
//
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -O2 -Wall -DANSWER=42 -c %s -o %t.o
// RUN: clang -O2 -Wall -DANSWER=42 -c %s -o %t.ref.o
// RUN: cmp %t.o %t.ref.o
// RUN: FileCheck %s --input-file=%t.o.emitrust.mlir
//
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang --version | FileCheck %s --check-prefix=VER
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -E %s -o %t.i
// RUN: not ls %t.i.emitrust.mlir
//
// The -DANSWER=42 macro must reach the import: the emitted module carries
// the constant.
// CHECK: emitrust.func @shim_answer
// CHECK: emitrust.constant <42 : i32>
//
// A rejected item (volatile) is recovered, not fatal: the artifact still
// contains the good function above.
//
// VER: clang version

int shim_answer(void) { return ANSWER; }

int shim_rejected(int n) {
  volatile int v = n; // rejects under strict import; recovered by the shim
  return v;
}
