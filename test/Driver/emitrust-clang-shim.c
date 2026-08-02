// FR-56 compiler-shim spike: `emitrust-clang` behaves as a drop-in C
// compiler. A `-c` compile delegates to the real clang (the object it
// produces is byte-identical to a plain-clang compile of the same command
// line) and side-emits one `<object>.emitrust.mlir` artifact holding the
// imported, converted emitrust module -- with `-D` macros provably reaching
// the import. Non-compile invocations (`--version`, `-E`) pass through
// verbatim and emit no artifact. An import rejection must NOT fail the
// build: the volatile local below would reject a strict import, but the
// shim's recover-mode import still lets clang's exit code (0) through.
// FR-57a: the shim imports in deferred-externals mode, so an extern global
// defined in some OTHER translation unit (ext_counter below) no longer
// costs the artifact: it becomes a declaration-only `emitrust.global`
// marked `emitrust.extern_decl` that the FR-58 link step must resolve.
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
// FR-57a: the cross-TU extern global survives as a marked declaration-only
// global (no initializer, FR-53 idiomatic spelling) instead of failing the
// import.
// CHECK: emitrust.global @EXT_COUNTER {emitrust.extern_decl} : i32
//
// VER: clang version

extern int ext_counter;

int shim_answer(void) { return ANSWER; }

int shim_reads_extern(void) { return ext_counter; }

int shim_rejected(int n) {
  volatile int v = n; // rejects under strict import; recovered by the shim
  return v;
}
