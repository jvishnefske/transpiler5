// RUN: emitrust-cc --recover --emit=import %s -o - 2>%t.err | FileCheck %s
// RUN: FileCheck %s --check-prefix=DIAG --input-file=%t.err
// RUN: emitrust-cc --recover --emit=rust %s -o - 2>/dev/null \
// RUN:   | FileCheck %s --check-prefix=RUST

// FR-42: the whole point of the stub is that a SUPPORTED caller of a
// rejected callee still imports and still compiles.
//
// The callee's signature is not re-derived for the stub —
// `importFunction` builds it on exactly the path the real import would have
// taken — which matters most where the signature is not a transliteration of
// the C prototype. Here the Phase-4 owner planner promotes `callee` to a
// METHOD of a synthesized owner struct, trading its `int *` parameter for a
// leading `&mut OwnerCallerLocal` receiver plus an i64 element index. An
// approximated stub signature would have emitted `fn callee(buf: &mut [i32],
// n: i32)` and broken the very call site this test pins.

void callee(int buf[4], int n) {
  volatile int v = n; // C99-7: rejects the body, not the signature.
  buf[0] = v;
}

int caller(void) {
  int local[4] = {0, 0, 0, 0};
  callee(local, 7);
  return local[0];
}

int main(void) { return caller(); }

// The stub carries the real planned signature — receiver, index, scalar —
// and the `method_of` placement attribute, and nothing else.
// CHECK-LABEL: func.func @callee(
// CHECK-SAME: !emitrust.mut_ref<!emitrust.struct<"OwnerCallerLocal">>
// CHECK-SAME: i64
// CHECK-SAME: i32)
// CHECK-SAME: emitrust.method_of = "OwnerCallerLocal"
// CHECK-NEXT: emitrust.call_opaque "unimplemented!"()
// CHECK-SAME: unsupported: volatile-qualified type
// CHECK-NEXT: return
// CHECK-NEXT: }

// The caller survives intact and still calls the stub against that very
// signature.
// CHECK-LABEL: func.func @caller
// CHECK: call @callee(%{{.*}}, %{{.*}}, %{{.*}}) {emitrust.method_call}
// CHECK-SAME: (!emitrust.mut_ref<!emitrust.struct<"OwnerCallerLocal">>, i64, i32) -> ()

// The emitted Rust is a well-formed method call on a well-formed method.
// RUST: fn caller() -> i32 {
// RUST: local.callee(
// RUST: impl OwnerCallerLocal {
// RUST-NEXT: fn callee(&mut self, _v0: i64, _v1: i32) {
// RUST-NEXT: unimplemented!("unsupported: volatile-qualified type");

// DIAG: warning: unsupported: volatile-qualified type (recovered: emitted an unimplemented!() stub with the mapped signature)
// DIAG: stubbed 'callee' [other]
