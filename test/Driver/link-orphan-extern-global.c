// FR-168 slice 1: an ORPHANED extern-global obligation must not be a link
// error, and a LIVE one must still be one.
//
// `pendingExternGlobals.try_emplace` (ImportCGlobals.cpp) is a permanent
// side effect of importing a body that READS an extern global.
// `RecoveryCheckpoint`/`rollbackTo` (ImportCRecovery.cpp) undo only the
// `func::FuncOp`s after the anchor, so when the body is rejected LATER --
// here by the inline asm, after the read was already imported -- the
// registration survives the rollback. `finalizeProject` then materialized an
// `emitrust.global {emitrust.extern_decl}` for it unconditionally, and the
// FR-58 merge demanded a definition that NOTHING in the whole program
// references. Measured over 501 systemd shards: 67 such orphans, and the 62
// that hard-errored at link were exactly them.
//
// The fix skips materializing a deferred obligation for a GLOBAL with no
// surviving IR use. The query runs on the pre-lowering `func` IR, where every
// reference to a global is a real `FlatSymbolRefAttr` symbol use
// (`emitrust.global_load` / `global_store` / `global_addr` / `global_place`);
// the only opaque-text initializer the importer ever builds is a fn-ptr
// `Some(<function>)`, which names a FUNCTION, never a global.
//
// Both ingredients below are load-bearing and defeated three earlier
// hand-reductions: the rejection must land AFTER the body registered the
// global (a signature-time rejection never registers it), and the symbol must
// be a GLOBAL (an extern FUNCTION prototype is correctly restored by
// `erasedExternalClones`).
//
// The orphan: `use_it` is dropped, so after recovery nothing references
// `vl_iface` and the solo link must succeed with no obligation left.
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
// RUN: emitrust-cc --link %t.main.o --emit=rust -o %t.rs
// RUN: FileCheck %s --check-prefix=ORPHAN < %t.rs
// ORPHAN-NOT: VL_IFACE
// ORPHAN: fn c_main
//
// The complement, and what stops the fix degenerating into "drop every
// unresolved global": a global obligation whose use SURVIVES keeps the hard,
// located link rejection.
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-orphan-extern-global-live.c -o %t.live.o
// RUN: not emitrust-cc --link %t.live.o --emit=rust -o %t.live.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=LIVE
// LIVE: link-orphan-extern-global-live.c:{{[0-9]+}}:{{[0-9]+}}: error: unresolved external 'VL_LIVE' at link
//
// FUNCTION obligations deliberately stay hard rejections this wave: after
// `ConvertToEmitRust` a call is `emitrust.call_opaque "name"` -- a STRING,
// not a `SymbolRefAttr` -- so "unreferenced" cannot be decided soundly for a
// function. That leg is already pinned by the `add` case in
// test/Driver/link-merge-errors.c and is not duplicated here.

struct Iface { int x; };
extern const struct Iface vl_iface;

int use_it(int n) {
  int r = vl_iface.x + n;
  __asm__ volatile ("nop"); /* rejected AFTER the read is imported */
  return r;
}

int main(void) { return 0; }
