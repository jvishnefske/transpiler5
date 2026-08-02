// FR-56 compiler-shim spike: `emitrust-clang` behaves as a drop-in C
// compiler. A `-c` compile delegates to the real clang (the object it
// produces matches a plain-clang compile of the same command line, modulo
// the added `.emitrust` section) and side-emits one
// `<object>.emitrust.mlirbc` bytecode artifact holding the imported,
// converted emitrust module -- with `-D` macros provably reaching the
// import. FR-57b embeds that same payload into the object file as a
// non-alloc `.emitrust` ELF section (the gllvm model), the sidecar staying
// as the non-ELF fallback. Non-compile invocations (`--version`, `-E`) pass
// through verbatim and emit no artifact. An import rejection must NOT fail
// the build: the volatile local below would reject a strict import, but the
// shim's recover-mode import still lets clang's exit code (0) through.
// FR-57a: the shim imports in deferred-externals mode, so an extern global
// defined in some OTHER translation unit (ext_counter below) no longer
// costs the artifact: it becomes a declaration-only `emitrust.global`
// marked `emitrust.extern_decl` that the FR-58 link step must resolve.
//
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -O2 -Wall -DANSWER=42 -c %s -o %t.o
// RUN: clang -O2 -Wall -DANSWER=42 -c %s -o %t.ref.o
//
// Delegation transparency: after stripping the shim's added section from
// both objects (a no-op strip on the reference, so both pass through the
// same objcopy rewriter), the bytes are identical.
// RUN: llvm-objcopy --remove-section .emitrust %t.o %t.stripped.o
// RUN: llvm-objcopy --remove-section .emitrust %t.ref.o %t.ref.stripped.o
// RUN: cmp %t.stripped.o %t.ref.stripped.o
//
// FR-57b: the bytecode sidecar round-trips through emitrust-opt to the same
// module the text artifact used to carry.
// RUN: emitrust-opt %t.o.emitrust.mlirbc -o - | FileCheck %s
//
// FR-57b: the object file carries the payload in its `.emitrust` section,
// byte-identical to the sidecar.
// RUN: llvm-objcopy --dump-section .emitrust=%t.payload %t.o
// RUN: cmp %t.payload %t.o.emitrust.mlirbc
//
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang --version | FileCheck %s --check-prefix=VER
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -E %s -o %t.i
// RUN: not ls %t.i.emitrust.mlirbc
//
// FR-57b cache-key canonicalization: workflow-only argument changes MUST
// NOT change the key. Two compiles of the same source to different `-o`
// paths (one with a depfile, exercising `-dependency-file`/`-MT`) log the
// SAME cc1-key; adding a macro logs a DIFFERENT one. The source-content
// hash is the same for all three.
// RUN: rm -f %t.log
// RUN: env EMITRUST_REAL_CC=clang EMITRUST_CLANG_LOG=%t.log emitrust-clang -O2 -Wall -DANSWER=42 -c %s -o %t.k1.o
// RUN: env EMITRUST_REAL_CC=clang EMITRUST_CLANG_LOG=%t.log emitrust-clang -O2 -Wall -DANSWER=42 -MD -MF %t.k2.d -c %s -o %t.k2.o
// RUN: grep "^cc1-key: " %t.log | sort -u | count 1
// RUN: env EMITRUST_REAL_CC=clang EMITRUST_CLANG_LOG=%t.log emitrust-clang -O2 -Wall -DANSWER=42 -DEXTRA=1 -c %s -o %t.k3.o
// RUN: grep "^cc1-key: " %t.log | sort -u | count 2
// RUN: grep "^src-hash: " %t.log | sort -u | count 1
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
