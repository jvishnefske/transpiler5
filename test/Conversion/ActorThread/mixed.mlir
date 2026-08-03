// FR-62 slice 5b: pins the MIXED module contract of emitrust-actor-thread —
// the invariants that (a) only the LISTED actor threads: an unlisted lifted
// actor (AccumulateActor) keeps its struct-typed driver local, its direct
// member accesses, and gains neither accessors nor an anchor; (b) a listed
// actor with NO struct_def in the module (GhostActor — the lift demoted it,
// so it was never lifted) is skipped SILENTLY, no warning, no trace — the
// composition rule "demoted actors silently stay same-thread"; and (c) the
// two coexist in one driver: the threaded handle construction and the
// same-thread member assign interleave without disturbing each other.
// stderr is folded into the FileCheck input so any stray warning fails the
// implicit-check-not pins.
// RUN: emitrust-opt %s --emitrust-actor-thread 2>&1 \
// RUN:   | FileCheck %s --implicit-check-not='GhostActor' \
// RUN:       --implicit-check-not='warning' \
// RUN:       --implicit-check-not='emitrust.actor_thread'

module attributes {
  emitrust.actor_thread = [{name = "CounterActor", mode = "threaded"},
                           {name = "GhostActor", mode = "threaded"}]} {
  emitrust.struct_def @CounterActor ["counter"] [i32]
  emitrust.impl "CounterActor" {
    emitrust.func @bump(%arg0: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> i32 {
      %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<!emitrust.struct<"CounterActor">>
      %1 = emitrust.member %0["counter"] : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<i32>
      %2 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
      emitrust.return %2 : i32
    }
  }
  // CHECK:      emitrust.actor_runtime @CounterActor mode = threaded

  emitrust.struct_def @AccumulateActor ["total"] [i32]
  emitrust.impl "AccumulateActor" {
    emitrust.func @accumulate(%arg0: !emitrust.mut_ref<!emitrust.struct<"AccumulateActor">>, %arg1: i32) -> i32 attributes {emitrust.param_names = ["", "x"]} {
      %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"AccumulateActor">>) -> !emitrust.lvalue<!emitrust.struct<"AccumulateActor">>
      %1 = emitrust.member %0["total"] : (!emitrust.lvalue<!emitrust.struct<"AccumulateActor">>) -> !emitrust.lvalue<i32>
      %2 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
      %3 = emitrust.add %2, %arg1 : i32
      emitrust.assign %1 = %3 : !emitrust.lvalue<i32>
      emitrust.return %3 : i32
    }
  }
  // The unlisted actor gains NO accessors and NO anchor.
  // CHECK-NOT:  emitrust.actor_runtime @AccumulateActor
  // CHECK-NOT:  @get_total
  // CHECK-NOT:  @set_total

  // CHECK:      emitrust.func @c_main() -> i32 {
  // CHECK:        emitrust.call_opaque "CounterActorHandle::spawn"
  // The unlisted actor's local keeps its struct type, member place, and
  // post-construction assign (slice 4's initializer composition, untouched
  // same-thread).
  // CHECK:        %[[A:.*]] = emitrust.variable named "accumulate_actor" : !emitrust.lvalue<!emitrust.struct<"AccumulateActor">>
  // CHECK-NEXT:   %[[T:.*]] = emitrust.member %[[A]]["total"]
  // CHECK:        emitrust.assign %[[T]] =
  // CHECK:        emitrust.method_call %{{.*}}["bump"] () : (!emitrust.lvalue<!emitrust.opaque<"CounterActorHandle">>) -> i32
  // CHECK:        emitrust.method_call %[[A]]["accumulate"] (%{{.*}}) : (!emitrust.lvalue<!emitrust.struct<"AccumulateActor">>, i32) -> i32
  // CHECK:        emitrust.method_call %{{.*}}["shutdown"] ()
  // CHECK-NEXT:   emitrust.return
  emitrust.func @c_main() -> i32 {
    %0 = emitrust.variable named "counter_actor" : !emitrust.lvalue<!emitrust.struct<"CounterActor">>
    %1 = emitrust.variable named "accumulate_actor" : !emitrust.lvalue<!emitrust.struct<"AccumulateActor">>
    %2 = emitrust.member %1["total"] : (!emitrust.lvalue<!emitrust.struct<"AccumulateActor">>) -> !emitrust.lvalue<i32>
    %3 = emitrust.constant <7 : i32> : i32
    emitrust.assign %2 = %3 : !emitrust.lvalue<i32>
    %4 = emitrust.method_call %0["bump"] () : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>) -> i32
    %5 = emitrust.method_call %1["accumulate"] (%4) : (!emitrust.lvalue<!emitrust.struct<"AccumulateActor">>, i32) -> i32
    emitrust.return %5 : i32
  }
}
