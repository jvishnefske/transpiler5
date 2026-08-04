// Emission of global accessors from inside an owner impl. The emitter's
// own global lookup (RustEmitter::emitGlobalLoad/emitGlobalStore via
// lookupGlobal in TranslateToRust.cpp) had the same nearest-symbol-table
// defect as the dialect verifiers: `emitrust.impl` is a SymbolTable, so a
// lookup rooted at a nested load/store never saw module-level
// `emitrust.global` ops. This pins that the emitter resolves the module's
// table and renders the usual thread_local Cell accessors inside the
// method body (see test/Dialect/EmitRust/global-in-impl.mlir for the
// verifier side of the same fix).
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK:      thread_local! {
// CHECK-NEXT:     static total: std::cell::Cell<i32> = std::cell::Cell::new(0);
// CHECK-NEXT: }
emitrust.global @total <0 : i32> : i32

// CHECK: impl Owner_main_values {
emitrust.impl "Owner_main_values" {
  // CHECK: fn bump(&mut self, v0: i32) {
  emitrust.func @bump(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner_main_values">>, %arg1: i32) {
    // The mutable-global load inlines into the store's set expression:
    // one statement carries both fixed lookups.
    // CHECK: total.with(|__emitrust_tl| __emitrust_tl.set(total.with(|__emitrust_tl| __emitrust_tl.get()) + v0));
    %0 = emitrust.global_load @total : i32
    %1 = emitrust.add %0, %arg1 : i32
    emitrust.global_store %1, @total : i32
    emitrust.return
  }
}
