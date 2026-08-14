// RUN: split-file %s %t
// RUN: rm -f %t/clean.mlir
// RUN: not emitrust-cc --emit=import --extra-arg=-fbogus-flag-xyz %t/clean.c -o %t/clean.mlir 2>&1 | FileCheck %s --check-prefix=BOGUS
// RUN: not ls %t/clean.mlir
// RUN: not emitrust-import-c %t/clean.c --extra-arg=-fbogus-flag-xyz -o - 2>&1 | FileCheck %s --check-prefix=BOGUS
// RUN: not emitrust-cc --emit=import --extra-arg=--bogus-double-dash %t/clean.c -o - 2>&1 | FileCheck %s --check-prefix=DDASH
// RUN: emitrust-cc --emit=import --extra-arg=-Wunused-variable %t/warn.c -o - 2>%t/warn.err | FileCheck %s --check-prefix=MODULE
// RUN: FileCheck %s --check-prefix=WARN --implicit-check-not=error: --input-file=%t/warn.err

// FR-68, no-compdb twin of compdb-gcc-flags.c's fbogus section: an unknown
// clang driver argument arriving via `--extra-arg` (a typo, or a GCC-only
// flag pasted from a build log) is a driver-LEVEL error that never reaches
// the ASTUnit's DiagnosticsEngine — historically the import proceeded, the
// tool exited 0, and a module was emitted anyway. That is the silent-recovery
// shape this repo forbids: the rejected argument may have been ABI-relevant.
// This test pins the closed gap on the no-compdb path for BOTH entry points
// (emitrust-cc's `importCProject` and emitrust-import-c's single-TU
// `importC`): clang's own error text survives byte-for-byte, the importer
// adds a rejection LOCATED on the offending translation unit, the exit code
// is nonzero, and no output file is written.
//
// FRONTIER (warn): warnings must NOT become failures. A user-supplied
// warning flag still imports cleanly with exit 0, a full module, and the
// diagnostic demoted to a warning carrying its `[-W...]` option name — the
// FR-67 tolerance flows (dropped `-Werror`, `-Wno-error=int-conversion`)
// depend on warning-severity diagnostics staying non-fatal.

//--- clean.c
int add_one(int x) { return x + 1; }

//--- warn.c
int with_warn(int x) { int u = 3; return x + 1; }

// Clang's own (unlocated) driver error first, then the importer's located
// rejection naming the offending TU and carrying clang's text.
// BOGUS: error: unknown argument: '-fbogus-flag-xyz'
// BOGUS: clean.c:1:1: error: clang error while building this translation unit: unknown argument: '-fbogus-flag-xyz'

// A typo'd double-dash option takes the same loud path.
// DDASH: error: unknown argument: '--bogus-double-dash'
// DDASH: clean.c:1:1: error: clang error while building this translation unit: unknown argument: '--bogus-double-dash'

// MODULE: func.func @with_warn

// WARN: warning: unused variable 'u' [-Wunused-variable]
