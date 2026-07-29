// RUN: emitrust-cc --recover --emit=import %s -o - 2>%t.err | FileCheck %s
// RUN: FileCheck %s --check-prefix=DIAG --input-file=%t.err
// RUN: not emitrust-cc --emit=import %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT

// FR-42: an item whose SIGNATURE cannot be mapped is dropped entirely —
// there is deliberately no approximated stub, because a stub with the wrong
// signature would mis-compile its callers instead of rejecting them.
//
// Dropping is safe precisely because the drop propagates: anything that
// referenced the dropped item rejects in turn ("call to unimported
// function") and is recovered by the same mechanism, one item at a time,
// with no special-case bookkeeping.

int supported(int x) { return x + 1; }

// `_Complex double` has no mapping, so the RETURN TYPE rejects while the
// signature is still being built. Nothing can be stubbed.
_Complex double unmappable(int x) { return (_Complex double)x; }

// Rejected only as a CONSEQUENCE of the drop: every construct in its own
// body is in the subset, and the one thing wrong with it is that the symbol
// it calls is no longer there. Its own signature maps, so it stubs.
int consumer(int x) {
  unmappable(x);
  return x;
}

int main(void) { return supported(1) + consumer(2); }

// The unmappable item leaves NO trace: no declaration, no stub, nothing.
// CHECK-NOT: @unmappable

// The supported function and the recovered consumer are both present, the
// consumer as a stub with its own (mappable) signature.
// CHECK-LABEL: func.func @supported
// CHECK-LABEL: func.func @consumer(
// CHECK-SAME: i32) -> i32 {
// CHECK-NEXT: emitrust.call_opaque "unimplemented!"()
// CHECK-NEXT: return
// CHECK-LABEL: func.func @c_main

// Two rejections, two different dispositions, both located and both warnings.
// DIAG: warning: unsupported type '_Complex double' (recovered: item dropped)
// DIAG: warning: unsupported: call to unimported function 'unmappable' (recovered: emitted an unimplemented!() stub with the mapped signature)
// DIAG-NOT: error:
// DIAG: recovered 2 rejected top-level items:
// DIAG: dropped 'unmappable' [other] unsupported type '_Complex double'
// DIAG: stubbed 'consumer' [other] unsupported: call to unimported function 'unmappable'
// DIAG: blocker tabulation (recovered items by tag):
// DIAG: other 2

// Without --recover the first rejection still stops the whole compile.
// STRICT: error: unsupported type '_Complex double'
