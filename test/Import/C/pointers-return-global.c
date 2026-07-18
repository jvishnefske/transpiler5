// RUN: emitrust-import-c %s | FileCheck %s

// CTS-S (00089): global-pointer returns. A data-pointer-returning
// function whose every return site yields the address of ONE mutable
// global (whole-object, cursor 0, never NULL) classifies as a
// single-global-base pointer RETURN region, extending the CTS-P2
// fn-address return classification. The pointer return is ERASED at the
// IR level: the function loses its result (no runtime pointer state
// travels), the call remains for its side effects, and every caller
// deref / `->member` access through the returned pointer routes to the
// global directly via the ordinary staged-copy + writeback machinery.
// Nullable, multi-base, member-address, and dangling-local returns stay
// rejected (see pointers-return-global-invalid.c).

struct S { int m; int n; };
struct S gs;
int calls;

// The returning function: side effect (calls++) plus the erased return.
struct S *go(void) {
  calls++;
  return &gs;
}
// CHECK: emitrust.global @gs
// CHECK: emitrust.global @calls
// CHECK-LABEL: func.func @go() {
// CHECK: emitrust.global_load @calls : i32
// CHECK: arith.addi
// CHECK: emitrust.global_store %{{.*}}, @calls : i32
// CHECK-NOT: emitrust.addr_of
// CHECK: return{{$}}

// A second function returning the same global's address.
struct S *again(void) { return &gs; }
// CHECK-LABEL: func.func @again() {
// CHECK-NOT: emitrust.addr_of
// CHECK: return{{$}}

// `go()->m = 5; return go()->m;` — the call survives (its side effect is
// observable), the write stages the global, assigns the member, and
// stores the whole value back; the read stages afresh. No runtime
// pointer state is materialized in the caller: no cell, no flag, no
// address value.
int write_read(void) {
  go()->m = 5;
  return go()->m;
}
// CHECK-LABEL: func.func @write_read
// CHECK-NOT: memref.alloca
// CHECK: call @go() : () -> ()
// CHECK: emitrust.global_load @gs : !emitrust.struct<"S">
// CHECK: emitrust.member %{{.*}}["m"]
// CHECK: emitrust.assign
// CHECK: emitrust.global_store %{{.*}}, @gs : !emitrust.struct<"S">
// CHECK: call @go() : () -> ()
// CHECK: emitrust.global_load @gs : !emitrust.struct<"S">
// CHECK: emitrust.member %{{.*}}["m"]
// CHECK: emitrust.load
// CHECK-NOT: emitrust.addr_of

// A chain through the second function, then a routed read mixed with a
// direct global read: the write through `again()->` is visible to both.
int chain(void) {
  again()->n = 7;
  return again()->n + gs.n;
}
// CHECK-LABEL: func.func @chain
// CHECK-NOT: memref.alloca
// CHECK: call @again() : () -> ()
// CHECK: emitrust.global_load @gs : !emitrust.struct<"S">
// CHECK: emitrust.member %{{.*}}["n"]
// CHECK: emitrust.assign
// CHECK: emitrust.global_store %{{.*}}, @gs : !emitrust.struct<"S">
// CHECK: call @again() : () -> ()
// CHECK: emitrust.global_load @gs : !emitrust.struct<"S">
// CHECK: emitrust.member %{{.*}}["n"]
// CHECK: emitrust.load
// CHECK: emitrust.global_load @gs : !emitrust.struct<"S">
// CHECK: emitrust.member %{{.*}}["n"]
// CHECK: emitrust.load
// CHECK: arith.addi
// CHECK-NOT: emitrust.addr_of
