// FR-141, the remaining two trigger shapes.
//
// The invariant this file pins: the orphan-owner-method sweep is keyed on
// the FACT (no `emitrust.struct_def` for the named owner), not on the shape
// of the owning function's rejection -- so all three ways of never reaching
// the promoted array's declaration statement are closed by one guard.
//
// Shape 1 (`ret_owner`): the owning function is DROPPED on a body-derived
// verdict raised before any statement is imported -- the returned-pointer
// classification, which is the sds `sdsfromlonglong`/`sdsll2str` pair that
// found this defect in the first place.
//
// Shape 2 (`stub_owner`): the owning function is STUBBED. Its body import
// died at a statement that precedes the array declaration, and the FR-42
// stub retry then emits a signature-carrying `unimplemented!()` body -- so
// the owner SYMBOL exists in the crate while the owner STRUCT does not.
// This is the one shape where the dangling impl sat next to a perfectly
// well-formed sibling item, which is what made it invisible.
//
// (Shape 3, the signature-level drop, and the negative control live in
// test/Driver/incremental-owner-orphan-impl.c and
// test/Driver/incremental-owner-struct-reached.c.)
//
// REQUIRES: cargo
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate --crate-type=lib \
// RUN:   --build 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/lib.rs
//
// Recovery OFF is unchanged: the first rejection is still a located error
// and no crate is written.
// RUN: not emitrust-cc --emit=crate %s -o %t.strict.crate --crate-type=lib 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT
// RUN: not ls %t.strict.crate

#include <ctype.h>

// --- Shape 1: owner DROPPED before its body is walked.
// WARN: :[[#@LINE+1]]:12: warning: unsupported: method of an owner struct that was never created: the owning function 'ret_owner' was rejected before its promoted local array 'abuf' was imported (recovered: item dropped)
static int fill_a(char *s, long long v) {
  s[0] = (char)('0' + (v % 10));
  return 1;
}

// STRICT: error: unsupported: returned pointer value
char *ret_owner(long long v) {
  char abuf[32];
  fill_a(abuf, v);
  return abuf;
}

// --- Shape 2: owner STUBBED, its body abandoned before the declaration.
// WARN: :[[#@LINE+1]]:12: warning: unsupported: method of an owner struct that was never created: the owning function 'stub_owner' was rejected before its promoted local array 'bbuf' was imported (recovered: item dropped)
static int fill_b(char *s, long long v) {
  s[1] = (char)('0' + (v % 7));
  return 2;
}

int stub_owner(long long v) {
  if (v == 0)
    return tolower(65);
  char bbuf[32];
  fill_b(bbuf, v);
  return bbuf[1];
}

// Four rejections, two of them the sweep's, each naming its own owner.
// WARN: recovered 4 rejected top-level items:
// WARN: dropped 'ret_owner' [returned-pointer] unsupported: returned pointer value
// WARN: stubbed 'stub_owner' [libc:tolower] unsupported: call to 'tolower' declared in a system header
// WARN: dropped 'tu0_fill_a' [owner-method-not-reached] unsupported: method of an owner struct that was never created: the owning function 'ret_owner' was rejected before its promoted local array 'abuf' was imported
// WARN: dropped 'tu0_fill_b' [owner-method-not-reached] unsupported: method of an owner struct that was never created: the owning function 'stub_owner' was rejected before its promoted local array 'bbuf' was imported

// The stub survives -- dropping the orphan methods must not take the
// unrelated item with them -- and no impl for either owner is left behind.
// RUST: pub fn stub_owner(
// RUST: unimplemented!(
// RUST-NOT: OwnerRetOwnerAbuf
// RUST-NOT: OwnerStubOwnerBbuf
// RUST-NOT: impl
// RUST-NOT: tu0_fill
