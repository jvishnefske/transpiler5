// FR-62 slice 5b: the `emitrust.actor_runtime` anchor round-trips
// losslessly. This pins the parse/print contract of the runtime anchor —
// the flat actor symbol plus the `mode = <threaded|async>` enum clause (the
// attr space carries BOTH flavors now even though async emission lands in a
// later slice) — and that a well-formed anchor verifies against the module:
// the symbol resolves to a struct_def with a non-empty impl of
// named-parameter receiver methods whose signatures are sendable
// (struct-field validity set), including a void method (unit reply), while
// a `&mut` of a DIFFERENT struct held by a module function stays legal (the
// borrow rule is per-anchored-actor, not global). Reprint through a second
// emitrust-opt must be byte-stable, exactly like the other custom-syntax
// ops.
// RUN: emitrust-opt %s | emitrust-opt | FileCheck %s

emitrust.struct_def @CounterActor ["counter", "table"] [i32, !emitrust.array<4xi32>]
emitrust.impl "CounterActor" {
  emitrust.func @bump(%arg0: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> i32 {
    %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<!emitrust.struct<"CounterActor">>
    %1 = emitrust.member %0["counter"] : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<i32>
    %2 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
    emitrust.return %2 : i32
  }
  emitrust.func @set_counter(%arg0: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>, %arg1: i32) attributes {emitrust.param_names = ["", "v"]} {
    %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<!emitrust.struct<"CounterActor">>
    %1 = emitrust.member %0["counter"] : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<i32>
    emitrust.assign %1 = %arg1 : !emitrust.lvalue<i32>
    emitrust.return
  }
}
// CHECK: emitrust.actor_runtime @CounterActor mode = threaded
emitrust.actor_runtime @CounterActor mode = threaded

emitrust.struct_def @LogActor ["lines"] [i32]
emitrust.impl "LogActor" {
  emitrust.func @note(%arg0: !emitrust.mut_ref<!emitrust.struct<"LogActor">>, %arg1: i32) attributes {emitrust.param_names = ["", "n"]} {
    %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"LogActor">>) -> !emitrust.lvalue<!emitrust.struct<"LogActor">>
    %1 = emitrust.member %0["lines"] : (!emitrust.lvalue<!emitrust.struct<"LogActor">>) -> !emitrust.lvalue<i32>
    emitrust.assign %1 = %arg1 : !emitrust.lvalue<i32>
    emitrust.return
  }
}
// CHECK: emitrust.actor_runtime @LogActor mode = async
emitrust.actor_runtime @LogActor mode = async

// A &mut of a struct with NO anchor stays legal in a module function: the
// borrow rule constrains anchored actors only.
emitrust.struct_def @Plain ["x"] [i32]
emitrust.func @touch(%arg0: !emitrust.mut_ref<!emitrust.struct<"Plain">>) {
  emitrust.return
}
