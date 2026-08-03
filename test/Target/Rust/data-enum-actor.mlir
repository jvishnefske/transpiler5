// FR-62 slice 5a integration: the exact actor-handle shape the threaded
// flavor will emit. A state struct plus an impl whose `&mut self` handler
// takes the closed message enum and statement-matches on it, mutating
// state through the payload bindings; a `&self` accessor result-matches
// and returns a payload as its tail expression; and a driver that
// constructs messages and feeds them through method calls. Pins that the
// three new ops compose with the existing struct_def/impl/method_call
// surface without touching it, and that the emitted text is
// deny-manifest-idiomatic (UpperCamel variants, snake_case fields,
// `_`-prefixed unused bindings, no unused parens).
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK:      #[derive(Clone, Copy)]
// CHECK-NEXT: enum Msg {
// CHECK-NEXT:     Quit,
// CHECK-NEXT:     Add { amount: i32 },
// CHECK-NEXT:     Move { x: i32, y: i32 },
// CHECK-NEXT: }
emitrust.data_enum_def @Msg ["Quit", "Add", "Move"] [[], ["amount"], ["x", "y"]] [[], [i32], [i32, i32]]

// CHECK:      #[derive(Clone, Copy, Default)]
// CHECK-NEXT: struct Counter {
// CHECK-NEXT:     count: i32,
// CHECK-NEXT: }
emitrust.struct_def @Counter ["count"] [i32]

// CHECK: impl Counter {
emitrust.impl "Counter" {
  // The statement-mode handler: one arm per message variant, state
  // mutated through the payload bindings.
  // CHECK:      fn handle(&mut self, v0: Msg) {
  // CHECK-NEXT:     match v0 {
  // CHECK-NEXT:         Msg::Quit => {
  // CHECK-NEXT:         }
  // CHECK-NEXT:         Msg::Add { amount: v1 } => {
  // CHECK-NEXT:             (*self).count = (*self).count + v1;
  // CHECK-NEXT:         }
  // CHECK-NEXT:         Msg::Move { x: v4, y: _v5 } => {
  // CHECK-NEXT:             (*self).count = v4;
  // CHECK-NEXT:         }
  // CHECK-NEXT:     }
  // CHECK-NEXT: }
  emitrust.func @handle(%self: !emitrust.mut_ref<!emitrust.struct<"Counter">>, %msg: !emitrust.data_enum<"Msg">) {
    emitrust.match %msg : !emitrust.data_enum<"Msg">
    case "Quit" {
    }
    case "Add" (%amount : i32) {
      %s = emitrust.deref %self : (!emitrust.mut_ref<!emitrust.struct<"Counter">>) -> !emitrust.lvalue<!emitrust.struct<"Counter">>
      %c = emitrust.member %s["count"] : (!emitrust.lvalue<!emitrust.struct<"Counter">>) -> !emitrust.lvalue<i32>
      %old = emitrust.load %c : (!emitrust.lvalue<i32>) -> i32
      %new = emitrust.add %old, %amount : i32
      emitrust.assign %c = %new : !emitrust.lvalue<i32>
    }
    case "Move" (%x : i32, %y : i32) {
      %s2 = emitrust.deref %self : (!emitrust.mut_ref<!emitrust.struct<"Counter">>) -> !emitrust.lvalue<!emitrust.struct<"Counter">>
      %c2 = emitrust.member %s2["count"] : (!emitrust.lvalue<!emitrust.struct<"Counter">>) -> !emitrust.lvalue<i32>
      emitrust.assign %c2 = %x : !emitrust.lvalue<i32>
    }
    emitrust.return
  }
  // The result-mode accessor: a `&self` method whose body is the bare
  // match expression (FR-61a tail fold), returning a payload.
  // CHECK:      fn payload(&self, v0: Msg) -> i32 {
  // CHECK-NEXT:     match v0 {
  // CHECK-NEXT:         Msg::Quit => {
  // CHECK-NEXT:             (*self).count
  // CHECK-NEXT:         }
  // CHECK-NEXT:         Msg::Add { amount: v3 } => {
  // CHECK-NEXT:             v3
  // CHECK-NEXT:         }
  // CHECK-NEXT:         Msg::Move { x: v4, y: v5 } => {
  // CHECK-NEXT:             v4 + v5
  // CHECK-NEXT:         }
  // CHECK-NEXT:     }
  // CHECK-NEXT: }
  emitrust.func @payload(%self: !emitrust.ref<!emitrust.struct<"Counter">>, %msg: !emitrust.data_enum<"Msg">) -> i32 {
    %r = emitrust.match %msg : !emitrust.data_enum<"Msg"> -> i32
    case "Quit" {
      %s = emitrust.deref %self : (!emitrust.ref<!emitrust.struct<"Counter">>) -> !emitrust.lvalue<!emitrust.struct<"Counter">>
      %c = emitrust.member %s["count"] : (!emitrust.lvalue<!emitrust.struct<"Counter">>) -> !emitrust.lvalue<i32>
      %v = emitrust.load %c : (!emitrust.lvalue<i32>) -> i32
      emitrust.yield %v : i32
    }
    case "Add" (%amount : i32) {
      emitrust.yield %amount : i32
    }
    case "Move" (%x : i32, %y : i32) {
      %sum = emitrust.add %x, %y : i32
      emitrust.yield %sum : i32
    }
    emitrust.return %r : i32
  }
// CHECK: }
}

// The driver: message construction feeding method calls, the final
// accessor call folding into the tail expression.
// CHECK:      fn c_main() -> i32 {
// CHECK-NEXT:     let mut v0: Counter = Counter::default();
// CHECK-NEXT:     let v2: Msg = Msg::Add { amount: 5i32 };
// CHECK-NEXT:     v0.handle(v2);
// CHECK-NEXT:     let v3: Msg = Msg::Quit;
// CHECK-NEXT:     v0.handle(v3);
// CHECK-NEXT:     let v6: Msg = Msg::Move { x: 2i32, y: 3i32 };
// CHECK-NEXT:     v0.payload(v6)
// CHECK-NEXT: }
emitrust.func @c_main() -> i32 {
  %counter = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Counter">>
  %five = emitrust.constant <5 : i32> : i32
  %add = emitrust.enum_variant @Msg "Add"(%five) : (i32) -> !emitrust.data_enum<"Msg">
  emitrust.method_call %counter["handle"] (%add) : (!emitrust.lvalue<!emitrust.struct<"Counter">>, !emitrust.data_enum<"Msg">) -> ()
  %quit = emitrust.enum_variant @Msg "Quit"() : () -> !emitrust.data_enum<"Msg">
  emitrust.method_call %counter["handle"] (%quit) : (!emitrust.lvalue<!emitrust.struct<"Counter">>, !emitrust.data_enum<"Msg">) -> ()
  %two = emitrust.constant <2 : i32> : i32
  %three = emitrust.constant <3 : i32> : i32
  %move = emitrust.enum_variant @Msg "Move"(%two, %three) : (i32, i32) -> !emitrust.data_enum<"Msg">
  %p = emitrust.method_call %counter["payload"] (%move) : (!emitrust.lvalue<!emitrust.struct<"Counter">>, !emitrust.data_enum<"Msg">) -> i32
  emitrust.return %p : i32
}
