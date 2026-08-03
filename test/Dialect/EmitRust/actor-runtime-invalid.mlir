// FR-62 slice 5b: the `emitrust.actor_runtime` verifier rejects every
// shape whose runtime synthesis could not compile or could not be sound,
// with located diagnostics — rejection is a feature, none of these may
// reach emission. Pinned surface: unresolved / wrong-kind actor symbol, a
// missing or empty impl (nothing to derive the message surface from, or a
// mailbox nothing can reach), a static/associated function (no receiver to
// delegate to), an unsendable parameter or result type (anything outside
// the struct-field validity set — a reference cannot cross the thread
// boundary), an unnamed parameter (no message field name to derive), a
// duplicate anchor (the synthesis is once-per-actor), and a function
// outside the actor's impl holding an `!emitrust.mut_ref` of the actor's
// struct (the E3 rule: state behind a mailbox has no cross-thread
// borrows).
// RUN: emitrust-opt %s --split-input-file --verify-diagnostics

// expected-error @+1 {{references '@Ghost' which is not an emitrust.struct_def}}
emitrust.actor_runtime @Ghost mode = threaded

// -----

emitrust.func @not_a_struct() {
  emitrust.return
}
// expected-error @+1 {{references '@not_a_struct' which is not an emitrust.struct_def}}
emitrust.actor_runtime @not_a_struct mode = threaded

// -----

emitrust.struct_def @A ["x"] [i32]
// expected-error @+1 {{actor '@A' has no emitrust.impl block to derive the message surface from}}
emitrust.actor_runtime @A mode = threaded

// -----

// (The empty-impl case — "an empty mailbox would spawn a thread nothing can
// reach" — is unrepresentable in textual IR: an `emitrust.impl` with no
// methods prints as an empty region, which the impl's own region constraint
// rejects at parse. The verifier check remains as a programmatic defense.)

// -----

emitrust.struct_def @A ["x"] [i32]
emitrust.impl "A" {
  emitrust.func @make() -> i32 attributes {emitrust.static_method} {
    %0 = emitrust.constant <0 : i32> : i32
    emitrust.return %0 : i32
  }
}
// expected-error @+1 {{method 'make' is a static/associated function; a mailbox delegates receiver methods only}}
emitrust.actor_runtime @A mode = threaded

// -----

emitrust.struct_def @A ["x"] [i32]
emitrust.impl "A" {
  emitrust.func @poke(%arg0: !emitrust.mut_ref<!emitrust.struct<"A">>, %arg1: !emitrust.ref<i32>) attributes {emitrust.param_names = ["", "p"]} {
    emitrust.return
  }
}
// expected-error @+1 {{method 'poke' parameter #1 type '!emitrust.ref<i32>' cannot cross the actor thread boundary}}
emitrust.actor_runtime @A mode = threaded

// -----

emitrust.struct_def @A ["x"] [i32]
emitrust.impl "A" {
  emitrust.func @view(%arg0: !emitrust.mut_ref<!emitrust.struct<"A">>) -> !emitrust.ref<i32> {
    %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"A">>) -> !emitrust.lvalue<!emitrust.struct<"A">>
    %1 = emitrust.member %0["x"] : (!emitrust.lvalue<!emitrust.struct<"A">>) -> !emitrust.lvalue<i32>
    %2 = emitrust.addr_of %1 : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
    emitrust.return %2 : !emitrust.ref<i32>
  }
}
// expected-error @+1 {{method 'view' result type '!emitrust.ref<i32>' cannot cross the actor thread boundary}}
emitrust.actor_runtime @A mode = threaded

// -----

emitrust.struct_def @A ["x"] [i32]
emitrust.impl "A" {
  emitrust.func @set(%arg0: !emitrust.mut_ref<!emitrust.struct<"A">>, %arg1: i32) {
    emitrust.return
  }
}
// expected-error @+1 {{method 'set' parameter #1 has no emitrust.param_names entry to name its message field}}
emitrust.actor_runtime @A mode = threaded

// -----

emitrust.struct_def @A ["x"] [i32]
emitrust.impl "A" {
  emitrust.func @bump(%arg0: !emitrust.mut_ref<!emitrust.struct<"A">>) {
    emitrust.return
  }
}
emitrust.actor_runtime @A mode = threaded
// expected-error @+1 {{duplicate actor_runtime anchor for '@A'}}
emitrust.actor_runtime @A mode = async

// -----

emitrust.struct_def @A ["x"] [i32]
emitrust.impl "A" {
  emitrust.func @bump(%arg0: !emitrust.mut_ref<!emitrust.struct<"A">>) {
    emitrust.return
  }
}
emitrust.func @cross_client(%arg0: !emitrust.mut_ref<!emitrust.struct<"A">>) {
  emitrust.return
}
// expected-error @+1 {{function 'cross_client' takes !emitrust.mut_ref of actor '@A' outside its impl; state behind a mailbox has no cross-thread borrows}}
emitrust.actor_runtime @A mode = threaded
