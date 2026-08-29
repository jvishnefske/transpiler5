// FR-141 NEGATIVE CONTROL: an owner method whose struct WAS created must
// survive untouched.
//
// The invariant this file pins: the end-of-TU orphan sweep added by FR-141
// rejects EXACTLY the methods whose owner struct does not exist, and not
// one method more. Its trigger is "the promoted array's declaration
// statement was never reached", which is a strictly narrower condition than
// "the owning function was rejected" -- an owning function rejected AFTER
// that declaration has already created the struct, and its methods are
// perfectly good code.
//
// This is the shape that stops the fix from degenerating into "drop every
// owner method of a rejected owner", which would silently cost real ported
// items. It is a companion to test/Driver/incremental-owner-orphan-impl.c
// (the same source, with the rejection moved BEFORE the declaration) and it
// must fail if anyone widens the sweep.
//
// REQUIRES: cargo
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate --crate-type=lib \
// RUN:   --build 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/lib.rs
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json

#include <ctype.h>

// The owner method: kept, with its receiver and its data member intact.
static int fill(char *s, long long v) {
  s[0] = (char)('0' + (v % 10));
  return 1;
}

// The owning function is rejected -- but only at its LAST statement, long
// after `char buf[32]` created the owner struct. It stubs; the struct and
// the method stay.
int make(long long v) {
  char buf[32];
  fill(buf, v);
  // WARN: :[[#@LINE+1]]:10: warning: unsupported: call to 'tolower' declared in a system header
  return tolower(buf[0]);
}

// Exactly ONE rejection: the sweep contributes nothing here.
// WARN: recovered 1 rejected top-level item:
// WARN: stubbed 'make' [libc:tolower] unsupported: call to 'tolower' declared in a system header
// WARN-NOT: owner-method-not-reached

// The struct IS defined, the method IS emitted into its impl, and the crate
// BUILDS (`--build` above).
// RUST: pub struct OwnerMakeBuf {
// RUST-NEXT: pub data: [i8; 32],
// RUST: impl OwnerMakeBuf {
// RUST-NEXT: fn tu0_fill(&mut self, {{.*}}) -> i32 {

// The method's progress row is byte-for-byte what it was before FR-141 --
// pinned here precisely so a WIDENED sweep (one that dropped every owner
// method of a rejected owner) would have to change it.
//
// It reads `missing` / `unreached-by-import` even though the method IS
// emitted, and that is a PRE-EXISTING, separately-owned reporting gap, not
// FR-141's doing: `collectEmittedSymbols` (ProgressReport.cpp) walks only
// the module body's direct children, so a method that lives inside an
// `emitrust.impl` is never in the emitted set and no owner method has ever
// been counted as ported. FR-141 fixes the OTHER direction of the same
// artifact -- an item reported missing whose body really was in the crate
// (see incremental-owner-orphan-impl.c) -- and deliberately leaves this one
// alone.
// JSON: "symbol": "tu0_fill"
// JSON-NEXT: "kind": "function"
// JSON-NEXT: "status": "missing"
// JSON: "blocker": "unreached-by-import"
