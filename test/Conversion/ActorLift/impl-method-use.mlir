// FR-116: pins the pass-level vetoes for a global (or an actor's arm/cross
// client) TOUCHED FROM A C++ METHOD BODY. `emitrust.impl` carries the MLIR
// SymbolTable trait, and `SymbolTable::getSymbolUses` /
// `symbolKnownUseEmpty` DO NOT traverse into nested symbol tables, so every
// use query in the actor-lift stack was structurally blind to method
// bodies: the locals veto saw zero uses of a global that a method reads,
// the plan localized it into the driver, and `deleteLiftedGlobals` erased
// it under a live reference — the MLIR VERIFIER, not the pass, reported the
// bug (`'emitrust.global_load' op 'G' does not reference a valid
// emitrust.global`). Two further manifestations reach rustc instead: an ARM
// renamed into an actor method leaves a method-body call to a free function
// that no longer exists (E0425), and a CROSS CLIENT gains one `&mut` per
// actor that the method-body call site does not pass (E0061). The fix is an
// impl-aware use scan inside the pass; the guarantee pinned here is that
// every one of these shapes DEMOTES with a located warning and keeps
// today's proven-correct thread-local form, never a verifier error and
// never broken Rust. Case (e) is the NEGATIVE pin that keeps the
// manifestation-4 token heuristic from widening: an arm's name appearing in
// a printf FORMAT STRING inside a method must NOT demote.
// RUN: emitrust-opt %s --split-input-file --emitrust-actor-lift \
// RUN:   > %t.out 2> %t.err
// RUN: FileCheck %s --check-prefix=WARN < %t.err
// RUN: FileCheck %s < %t.out
// The demotions are EXACTLY these five: case (e) must contribute none, and
// only case (e) may leave an actor struct standing.
// RUN: grep "warning: actor lift: demoted" %t.err | count 5
// RUN: grep "emitrust.struct_def @GActor" %t.out | count 1
// RUN: not grep "LeftActor" %t.out
// RUN: not grep "RightActor" %t.out

// The warnings, in case order, each LOCATED on the offending op.
// WARN:      warning: actor lift: demoted g: global 'G' has a use outside the driver
// WARN-NEXT:   emitrust.global @G
// WARN:      warning: actor lift: demoted GActor: arm 'bump' is referenced from a method body
// WARN-NEXT:   emitrust.call_opaque "bump"
// WARN:      warning: actor lift: demoted LeftActor: cross client 'observe' is referenced from a method body
// WARN-NEXT:   emitrust.call_opaque "observe"
// WARN:      warning: actor lift: demoted RightActor: cross client 'observe' is referenced from a method body
// WARN-NEXT:   emitrust.call_opaque "observe"
// WARN:      warning: actor lift: demoted GActor: arm 'bump' is referenced from a method body
// WARN-NEXT:   emitrust.constant <#emitrust.opaque<"Some(bump)">>

// (a) The repro: the plan made G a DRIVER LOCAL because the only access is
// inside `emitrust.impl "S"`, which the plan cannot see. The impl-aware
// scan must find that load, demote the local, and leave the global — and
// the method's load of it — exactly as they came in.
// CHECK-LABEL: module {
// CHECK:      emitrust.global @G <7 : i32> : i32
// CHECK-NOT:  emitrust.variable named "g"
// CHECK:      emitrust.impl "S"
// CHECK:        emitrust.func @s_peek
// CHECK:          emitrust.global_load @G : i32
module attributes {emitrust.actor_locals = [{global = "G", name = "g"}]} {
  emitrust.global @G <7 : i32> : i32
  emitrust.struct_def @S ["v"] [i32]
  emitrust.func @c_main(%arg0: i32) -> i32 attributes {emitrust.actor_driver, emitrust.param_names = ["argc"]} {
    %0 = emitrust.constant <0 : i32> : i32
    %1 = emitrust.variable named "s" : !emitrust.lvalue<!emitrust.struct<"S">>
    emitrust.method_call %1["s_new"] (%arg0) : (!emitrust.lvalue<!emitrust.struct<"S">>, i32) -> ()
    %2 = emitrust.method_call %1["s_peek"] () : (!emitrust.lvalue<!emitrust.struct<"S">>) -> i32
    emitrust.call_opaque "println!"(%2) {args = ["{}", 0 : index]} : (i32) -> ()
    emitrust.return %0 : i32
  }
  emitrust.impl "S" {
    emitrust.func @s_new(%arg0: !emitrust.mut_ref<!emitrust.struct<"S">>, %arg1: i32) attributes {emitrust.param_names = ["", "x"]} {
      %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.struct<"S">>
      %1 = emitrust.member %0["v"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<i32>
      emitrust.assign %1 = %arg1 : !emitrust.lvalue<i32>
      emitrust.return
    }
    emitrust.func @s_peek(%arg0: !emitrust.mut_ref<!emitrust.struct<"S">>) -> i32 {
      %0 = emitrust.global_load @G : i32
      %1 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.struct<"S">>
      %2 = emitrust.member %1["v"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<i32>
      %3 = emitrust.load %2 : (!emitrust.lvalue<i32>) -> i32
      %4 = emitrust.add %0, %3 : i32
      emitrust.return %4 : i32
    }
  }
}

// -----

// (b) 2nd manifestation: G is genuinely owned by the arm `bump`, but `bump`
// is CALLED FROM A METHOD BODY. Renaming it into `GActor::bump` would leave
// the impl calling a free function that no longer exists (E0425), so the
// actor demotes and `bump` stays a module-level function.
// CHECK-LABEL: module {
// CHECK:      emitrust.global @G <7 : i32> : i32
// CHECK-NEXT: emitrust.func @bump
// CHECK-NOT:    emitrust.actor_arm
// CHECK:        emitrust.global_load @G : i32
// CHECK:      emitrust.impl "S"
// CHECK:        emitrust.call_opaque "bump"
module attributes {emitrust.actor_lift = [{fields = ["g"], globals = ["G"], name = "GActor", var = "g_actor"}]} {
  emitrust.global @G <7 : i32> : i32
  emitrust.func @bump(%arg0: i32) -> i32 attributes {emitrust.actor_arm = "GActor", emitrust.param_names = ["d"]} {
    %0 = emitrust.global_load @G : i32
    %1 = emitrust.add %0, %arg0 : i32
    emitrust.global_store %1, @G : i32
    %2 = emitrust.global_load @G : i32
    emitrust.return %2 : i32
  }
  emitrust.struct_def @S ["v"] [i32]
  emitrust.func @c_main(%arg0: i32) -> i32 attributes {emitrust.actor_driver, emitrust.param_names = ["argc"]} {
    %0 = emitrust.constant <0 : i32> : i32
    %1 = emitrust.variable named "s" : !emitrust.lvalue<!emitrust.struct<"S">>
    emitrust.method_call %1["s_new"] (%arg0) : (!emitrust.lvalue<!emitrust.struct<"S">>, i32) -> ()
    %2 = emitrust.method_call %1["s_step"] () : (!emitrust.lvalue<!emitrust.struct<"S">>) -> i32
    emitrust.call_opaque "println!"(%2) {args = ["{}", 0 : index]} : (i32) -> ()
    emitrust.return %0 : i32
  }
  emitrust.impl "S" {
    emitrust.func @s_new(%arg0: !emitrust.mut_ref<!emitrust.struct<"S">>, %arg1: i32) attributes {emitrust.param_names = ["", "x"]} {
      %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.struct<"S">>
      %1 = emitrust.member %0["v"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<i32>
      emitrust.assign %1 = %arg1 : !emitrust.lvalue<i32>
      emitrust.return
    }
    emitrust.func @s_step(%arg0: !emitrust.mut_ref<!emitrust.struct<"S">>) -> i32 {
      %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.struct<"S">>
      %1 = emitrust.member %0["v"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<i32>
      %2 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
      %3 = emitrust.call_opaque "bump"(%2) : (i32) -> i32
      emitrust.return %3 : i32
    }
  }
}

// -----

// (c) 3rd manifestation: a CROSS CLIENT called from a method body. The lift
// would prepend one `&mut` parameter per actor to `observe` while the
// method's call site still passes one argument (E0061), so BOTH actors
// demote — one warning each, in actor order.
// CHECK-LABEL: module {
// CHECK:      emitrust.global @LEFT : i32
// CHECK-NEXT: emitrust.global @RIGHT : i32
// CHECK:      emitrust.func @observe(%{{.*}}: i32) -> i32
// CHECK:        emitrust.call_opaque "get_left"
// CHECK:      emitrust.impl "S"
// CHECK:        emitrust.call_opaque "observe"
module attributes {
  emitrust.actor_lift = [{name = "LeftActor", var = "left_actor",
                          globals = ["LEFT"], fields = ["left"]},
                         {name = "RightActor", var = "right_actor",
                          globals = ["RIGHT"], fields = ["right"]}]} {
  emitrust.global @LEFT : i32
  emitrust.global @RIGHT : i32
  emitrust.func @get_left() -> i32 attributes {emitrust.actor_arm = "LeftActor"} {
    %0 = emitrust.global_load @LEFT : i32
    emitrust.return %0 : i32
  }
  emitrust.func @get_right() -> i32 attributes {emitrust.actor_arm = "RightActor"} {
    %0 = emitrust.global_load @RIGHT : i32
    emitrust.return %0 : i32
  }
  emitrust.func @observe(%arg0: i32) -> i32
      attributes {emitrust.actor_cross = ["LeftActor", "RightActor"],
                  emitrust.param_names = ["k"]} {
    %0 = emitrust.call_opaque "get_left"() : () -> i32
    %1 = emitrust.call_opaque "get_right"() : () -> i32
    %2 = emitrust.add %0, %1 : i32
    %3 = emitrust.add %2, %arg0 : i32
    emitrust.return %3 : i32
  }
  emitrust.struct_def @S ["v"] [i32]
  emitrust.func @c_main(%arg0: i32) -> i32 attributes {emitrust.actor_driver, emitrust.param_names = ["argc"]} {
    %0 = emitrust.constant <0 : i32> : i32
    %1 = emitrust.variable named "s" : !emitrust.lvalue<!emitrust.struct<"S">>
    %2 = emitrust.method_call %1["s_go"] () : (!emitrust.lvalue<!emitrust.struct<"S">>) -> i32
    emitrust.call_opaque "println!"(%2) {args = ["{}", 0 : index]} : (i32) -> ()
    emitrust.return %0 : i32
  }
  emitrust.impl "S" {
    emitrust.func @s_go(%arg0: !emitrust.mut_ref<!emitrust.struct<"S">>) -> i32 {
      %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.struct<"S">>
      %1 = emitrust.member %0["v"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<i32>
      %2 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
      %3 = emitrust.call_opaque "observe"(%2) : (i32) -> i32
      emitrust.return %3 : i32
    }
  }
}

// -----

// (d) 4th manifestation: a method body takes a FUNCTION POINTER to an arm.
// The reference is opaque TEXT (`Some(bump)`), so the veto is an
// identifier-token match inside the impl's opaque attributes; without it
// the emitted Rust names a free `bump` that the lift has renamed (E0425).
// CHECK-LABEL: module {
// CHECK:      emitrust.global @G <7 : i32> : i32
// CHECK-NEXT: emitrust.func @bump
// CHECK:        emitrust.global_store %{{.*}}, @G : i32
// CHECK:      emitrust.impl "S"
// CHECK:        emitrust.constant <#emitrust.opaque<"Some(bump)">>
module attributes {emitrust.actor_lift = [{fields = ["g"], globals = ["G"], name = "GActor", var = "g_actor"}]} {
  emitrust.global @G <7 : i32> : i32
  emitrust.func @bump(%arg0: i32) -> i32 attributes {emitrust.actor_arm = "GActor", emitrust.param_names = ["d"]} {
    %0 = emitrust.global_load @G : i32
    %1 = emitrust.add %0, %arg0 : i32
    emitrust.global_store %1, @G : i32
    emitrust.return %1 : i32
  }
  emitrust.struct_def @S ["v"] [i32]
  emitrust.func @c_main(%arg0: i32) -> i32 attributes {emitrust.actor_driver, emitrust.param_names = ["argc"]} {
    %0 = emitrust.constant <0 : i32> : i32
    %1 = emitrust.variable named "s" : !emitrust.lvalue<!emitrust.struct<"S">>
    %2 = emitrust.method_call %1["s_go"] () : (!emitrust.lvalue<!emitrust.struct<"S">>) -> i32
    emitrust.call_opaque "println!"(%2) {args = ["{}", 0 : index]} : (i32) -> ()
    emitrust.return %0 : i32
  }
  emitrust.impl "S" {
    emitrust.func @s_go(%arg0: !emitrust.mut_ref<!emitrust.struct<"S">>) -> i32 {
      %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.struct<"S">>
      %1 = emitrust.member %0["v"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<i32>
      %2 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
      %3 = emitrust.constant <#emitrust.opaque<"Some(bump)">> : !emitrust.fn_ptr<(i32) -> i32>
      %4 = emitrust.call_indirect %3(%2) : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
      emitrust.return %4 : i32
    }
  }
}

// -----

// (e) NEGATIVE: the manifestation-4 veto is a token match on opaque text,
// so it must be provably narrow. Here the arm's name `bump` appears only in
// a printf FORMAT STRING inside a method body — format strings live in an
// `args` StringAttr array, never an OpaqueAttr — and the actor must still
// lift in full. If this case ever demotes, the heuristic has widened.
// CHECK-LABEL: module {
// CHECK:      emitrust.struct_def @GActor ["g"] [i32]
// CHECK-NEXT: emitrust.impl "GActor"
// CHECK-NEXT:   emitrust.func @bump
// CHECK:      emitrust.impl "S"
// CHECK:        emitrust.call_opaque "println!"(%{{.*}}) {args = ["bump {}", 0 : index]}
module attributes {emitrust.actor_lift = [{fields = ["g"], globals = ["G"], name = "GActor", var = "g_actor"}]} {
  emitrust.global @G <7 : i32> : i32
  emitrust.func @bump(%arg0: i32) -> i32 attributes {emitrust.actor_arm = "GActor", emitrust.param_names = ["d"]} {
    %0 = emitrust.global_load @G : i32
    %1 = emitrust.add %0, %arg0 : i32
    emitrust.global_store %1, @G : i32
    emitrust.return %1 : i32
  }
  emitrust.struct_def @S ["v"] [i32]
  emitrust.func @c_main(%arg0: i32) -> i32 attributes {emitrust.actor_driver, emitrust.param_names = ["argc"]} {
    %0 = emitrust.constant <0 : i32> : i32
    %1 = emitrust.variable named "s" : !emitrust.lvalue<!emitrust.struct<"S">>
    emitrust.method_call %1["s_say"] () : (!emitrust.lvalue<!emitrust.struct<"S">>) -> ()
    %2 = emitrust.call_opaque "bump"(%arg0) : (i32) -> i32
    emitrust.call_opaque "println!"(%2) {args = ["{}", 0 : index]} : (i32) -> ()
    emitrust.return %0 : i32
  }
  emitrust.impl "S" {
    emitrust.func @s_say(%arg0: !emitrust.mut_ref<!emitrust.struct<"S">>) attributes {emitrust.param_names = [""]} {
      %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.struct<"S">>
      %1 = emitrust.member %0["v"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<i32>
      %2 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
      emitrust.call_opaque "println!"(%2) {args = ["bump {}", 0 : index]} : (i32) -> ()
      emitrust.return
    }
  }
}
