// FR-62 slice 5b: pins the ELIGIBILITY VETOES of emitrust-actor-thread —
// the invariant that a lifted-but-thread-ineligible actor stays lifted
// SAME-THREAD with a located `actor plan: <actor> stays same-thread:
// <reason>` warning and its IR left completely untouched: no accessor is
// synthesized, no anchor appears (--implicit-check-not on both), the
// driver local keeps its struct type. Demote-from-threading is not
// demote-from-lifting. Case order: (a) a cross-actor client holding a
// &mut of the actor's struct (the E3 landmine rule), (b) an unsendable
// method signature (a reference parameter), (b') an unnamed parameter (no
// message field to derive), (c) a synthesized accessor name colliding
// with an existing method, (d) a driver access with no accessor form (a
// whole-array member load). The anchor op is created only after a full
// rewrite, so the module-wide implicit-check-not on it pins that NO case
// threaded (case c's pre-existing @get_counter method survives verbatim,
// which is why the pin is the anchor, not the accessor spelling).
// RUN: emitrust-opt %s --split-input-file --emitrust-actor-thread 2>&1 \
// RUN:   | FileCheck %s --implicit-check-not='emitrust.actor_runtime'

// The veto warnings are located diagnostics; the diagnostic stream
// precedes the printed modules in the merged output (the demote.mlir
// precedent), so all five are pinned here in case order.
// CHECK: warning: actor plan: CounterActor stays same-thread: function 'report' holds a &mut reference across the thread boundary
// CHECK: warning: actor plan: CounterActor stays same-thread: method 'peek' has an unsendable signature
// CHECK: warning: actor plan: CounterActor stays same-thread: method 'add' has an unsendable signature
// CHECK: warning: actor plan: CounterActor stays same-thread: accessor 'get_counter' collides with an existing method
// CHECK: warning: actor plan: TableActor stays same-thread: driver access to field 'table' has no accessor form

// (a) Cross-client &mut: the borrow cannot cross the thread boundary.
// CHECK:      emitrust.func @report
// CHECK:      %{{.*}} = emitrust.variable named "counter_actor" : !emitrust.lvalue<!emitrust.struct<"CounterActor">>
module attributes {
  emitrust.actor_thread = [{name = "CounterActor", mode = "threaded"}]} {
  emitrust.struct_def @CounterActor ["counter"] [i32]
  emitrust.impl "CounterActor" {
    emitrust.func @bump(%arg0: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> i32 {
      %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<!emitrust.struct<"CounterActor">>
      %1 = emitrust.member %0["counter"] : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<i32>
      %2 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
      emitrust.return %2 : i32
    }
  }
  emitrust.func @report(%arg0: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>) {
    emitrust.return
  }
  emitrust.func @c_main() -> i32 {
    %0 = emitrust.variable named "counter_actor" : !emitrust.lvalue<!emitrust.struct<"CounterActor">>
    %1 = emitrust.method_call %0["bump"] () : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>) -> i32
    emitrust.return %1 : i32
  }
}

// -----

// (b) Unsendable signature: a reference parameter cannot become a message
// field.
// CHECK:      %{{.*}} = emitrust.variable named "counter_actor" : !emitrust.lvalue<!emitrust.struct<"CounterActor">>
module attributes {
  emitrust.actor_thread = [{name = "CounterActor", mode = "threaded"}]} {
  emitrust.struct_def @CounterActor ["counter"] [i32]
  emitrust.impl "CounterActor" {
    emitrust.func @peek(%arg0: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>, %arg1: !emitrust.ref<i32>) attributes {emitrust.param_names = ["", "p"]} {
      emitrust.return
    }
  }
  emitrust.func @c_main() -> i32 {
    %0 = emitrust.variable named "counter_actor" : !emitrust.lvalue<!emitrust.struct<"CounterActor">>
    %1 = emitrust.constant <0 : i32> : i32
    emitrust.return %1 : i32
  }
}

// -----

// (b') Unnamed parameter: same unsendable-signature veto (no
// emitrust.param_names slot to derive the message field from).
// CHECK:      %{{.*}} = emitrust.variable named "counter_actor" : !emitrust.lvalue<!emitrust.struct<"CounterActor">>
module attributes {
  emitrust.actor_thread = [{name = "CounterActor", mode = "threaded"}]} {
  emitrust.struct_def @CounterActor ["counter"] [i32]
  emitrust.impl "CounterActor" {
    emitrust.func @add(%arg0: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>, %arg1: i32) {
      emitrust.return
    }
  }
  emitrust.func @c_main() -> i32 {
    %0 = emitrust.variable named "counter_actor" : !emitrust.lvalue<!emitrust.struct<"CounterActor">>
    %1 = emitrust.constant <0 : i32> : i32
    emitrust.return %1 : i32
  }
}

// -----

// (c) Accessor-name collision: the driver's direct read of `counter` needs
// a synthesized get_counter, but the impl already defines one.
// CHECK:      %[[V:.*]] = emitrust.variable named "counter_actor" : !emitrust.lvalue<!emitrust.struct<"CounterActor">>
// CHECK-NEXT: %{{.*}} = emitrust.member %[[V]]["counter"]
module attributes {
  emitrust.actor_thread = [{name = "CounterActor", mode = "threaded"}]} {
  emitrust.struct_def @CounterActor ["counter"] [i32]
  emitrust.impl "CounterActor" {
    emitrust.func @get_counter(%arg0: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> i32 {
      %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<!emitrust.struct<"CounterActor">>
      %1 = emitrust.member %0["counter"] : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<i32>
      %2 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
      emitrust.return %2 : i32
    }
  }
  emitrust.func @c_main() -> i32 {
    %0 = emitrust.variable named "counter_actor" : !emitrust.lvalue<!emitrust.struct<"CounterActor">>
    %1 = emitrust.member %0["counter"] : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<i32>
    %2 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
    emitrust.return %2 : i32
  }
}

// -----

// (d) No accessor form: a whole-array member load is a snapshot the
// accessor rules deliberately exclude.
// CHECK:      %{{.*}} = emitrust.variable named "table_actor" : !emitrust.lvalue<!emitrust.struct<"TableActor">>
module attributes {
  emitrust.actor_thread = [{name = "TableActor", mode = "threaded"}]} {
  emitrust.struct_def @TableActor ["table"] [!emitrust.array<4xi32>]
  emitrust.impl "TableActor" {
    emitrust.func @clear(%arg0: !emitrust.mut_ref<!emitrust.struct<"TableActor">>) {
      emitrust.return
    }
  }
  emitrust.func @c_main() -> i32 {
    %0 = emitrust.variable named "table_actor" : !emitrust.lvalue<!emitrust.struct<"TableActor">>
    %1 = emitrust.member %0["table"] : (!emitrust.lvalue<!emitrust.struct<"TableActor">>) -> !emitrust.lvalue<!emitrust.array<4xi32>>
    %2 = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
    %3 = emitrust.load %1 : (!emitrust.lvalue<!emitrust.array<4xi32>>) -> !emitrust.array<4xi32>
    emitrust.assign %2 = %3 : !emitrust.lvalue<!emitrust.array<4xi32>>
    %4 = emitrust.constant <0 : i32> : i32
    emitrust.return %4 : i32
  }
}
