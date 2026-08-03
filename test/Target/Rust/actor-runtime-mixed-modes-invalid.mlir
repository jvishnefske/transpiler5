// FR-62 slice 5c: a module mixing actor runtime MODES cannot be emitted.
// The shared `mod actor_rt` epilogue exists once per crate and its text is
// flavor-specific (std::sync::mpsc + std::thread vs tokio); two anchors
// disagreeing on mode would need two incompatible epilogues under one
// name. The driver never produces this shape (--actor-mode is global), so
// reaching emission with it is a hand-written or future-per-actor module —
// and it must fail LOUDLY at the second flavor's anchor, never silently
// emit one flavor's runtime for the other's wrappers.
// RUN: not emitrust-translate --mlir-to-rust %s 2>&1 | FileCheck %s

emitrust.struct_def @LeftActor ["hits"] [i32]
emitrust.impl "LeftActor" {
  emitrust.func @ping(%arg0: !emitrust.mut_ref<!emitrust.struct<"LeftActor">>) {
    %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"LeftActor">>) -> !emitrust.lvalue<!emitrust.struct<"LeftActor">>
    %1 = emitrust.member %0["hits"] : (!emitrust.lvalue<!emitrust.struct<"LeftActor">>) -> !emitrust.lvalue<i32>
    %2 = emitrust.constant <1 : i32> : i32
    emitrust.assign %1 = %2 : !emitrust.lvalue<i32>
    emitrust.return
  }
}
emitrust.actor_runtime @LeftActor mode = threaded

emitrust.struct_def @RightActor ["hits"] [i32]
emitrust.impl "RightActor" {
  emitrust.func @pong(%arg0: !emitrust.mut_ref<!emitrust.struct<"RightActor">>) {
    %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"RightActor">>) -> !emitrust.lvalue<!emitrust.struct<"RightActor">>
    %1 = emitrust.member %0["hits"] : (!emitrust.lvalue<!emitrust.struct<"RightActor">>) -> !emitrust.lvalue<i32>
    %2 = emitrust.constant <1 : i32> : i32
    emitrust.assign %1 = %2 : !emitrust.lvalue<i32>
    emitrust.return
  }
}
// CHECK: error: actor 'RightActor' mode disagrees with actor 'LeftActor': one module carries one actor_rt runtime flavor
emitrust.actor_runtime @RightActor mode = async
