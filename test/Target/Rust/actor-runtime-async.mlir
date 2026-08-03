// FR-62 slice 5c: Rust rendering of the `emitrust.actor_runtime` anchor in
// mode = async — the tokio task flavor (E4). The derivation is IDENTICAL to
// the threaded flavor (B-prime: everything comes from the referenced impl,
// no reified message enum in IR); only the substrate changes, exactly the
// spike's measured deltas: the <Actor>Msg reply field is a
// tokio::sync::oneshot::Sender (a per-call typed reply channel, `Sender<()>`
// keeping a void call synchronous under the same rule), `spawn` moves the
// state onto a tokio::task behind an UNBOUNDED mpsc mailbox (send never
// awaits, so the wrapper's only suspension point is the reply), each wrapper
// is an `async fn` whose `self.call(..)` IMMEDIATELY awaits (at most one
// message in flight — effect order equals program order on the
// current_thread runtime), every driver method_call on an async handle
// appends `.await`, and the driver function containing those calls renders
// `async fn`. The shared `mod actor_rt` epilogue is the tokio flavor
// (UnboundedSender + tokio::task::JoinHandle; reap re-raises the actor's
// own payload via JoinError::into_panic), appended ONCE per module.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

emitrust.struct_def @CounterActor ["counter", "table"] [i32, !emitrust.array<4xi32>]
emitrust.impl "CounterActor" {
  emitrust.func @bump(%arg0: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> i32 {
    %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<!emitrust.struct<"CounterActor">>
    %1 = emitrust.member %0["counter"] : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<i32>
    %2 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
    %3 = emitrust.constant <1 : i32> : i32
    %4 = emitrust.add %2, %3 : i32
    emitrust.assign %1 = %4 : !emitrust.lvalue<i32>
    %5 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
    emitrust.return %5 : i32
  }
  emitrust.func @set_table(%arg0: !emitrust.mut_ref<!emitrust.struct<"CounterActor">>, %arg1: i32, %arg2: i32) attributes {emitrust.param_names = ["", "i", "v"]} {
    %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<!emitrust.struct<"CounterActor">>
    %1 = emitrust.member %0["table"] : (!emitrust.lvalue<!emitrust.struct<"CounterActor">>) -> !emitrust.lvalue<!emitrust.array<4xi32>>
    %2 = emitrust.subscript %1[%arg1] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
    emitrust.assign %2 = %arg2 : !emitrust.lvalue<i32>
    emitrust.return
  }
}

// The message enum: variant = UpperCamel(method); payload fields = named
// params; the reply channel is the trailing field, typed by the result —
// a tokio oneshot in this flavor (`Sender<()>` for the void set_table).
// CHECK:      enum CounterActorMsg {
// CHECK-NEXT:     Bump { reply: tokio::sync::oneshot::Sender<i32> },
// CHECK-NEXT:     SetTable { i: i32, v: i32, reply: tokio::sync::oneshot::Sender<()> },
// CHECK-NEXT: }
// CHECK-NEXT: type CounterActorHandle = actor_rt::Handle<CounterActorMsg>;

// The spawn mailbox loop: moved state on a tokio task, unbounded receiver,
// one arm per variant, delegation + reply. oneshot's send returns the
// unsent value itself on failure (no Debug bound is available), so the
// loud-failure form is an is_err check, not the threaded flavor's expect.
// CHECK-NEXT: impl actor_rt::Handle<CounterActorMsg> {
// CHECK-NEXT:     fn spawn(mut state: CounterActor) -> Self {
// CHECK-NEXT:         let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<CounterActorMsg>();
// CHECK-NEXT:         let join = tokio::task::spawn(async move {
// CHECK-NEXT:             while let Some(msg) = rx.recv().await {
// CHECK-NEXT:                 match msg {
// CHECK-NEXT:                     CounterActorMsg::Bump { reply } => {
// CHECK-NEXT:                         if reply.send(state.bump()).is_err() {
// CHECK-NEXT:                             panic!("actor caller dropped reply receiver");
// CHECK-NEXT:                         }
// CHECK-NEXT:                     }
// CHECK-NEXT:                     CounterActorMsg::SetTable { i, v, reply } => {
// CHECK-NEXT:                         if reply.send(state.set_table(i, v)).is_err() {
// CHECK-NEXT:                             panic!("actor caller dropped reply receiver");
// CHECK-NEXT:                         }
// CHECK-NEXT:                     }
// CHECK-NEXT:                 }
// CHECK-NEXT:             }
// CHECK-NEXT:         });
// CHECK-NEXT:         Self { tx, join: Some(join) }
// CHECK-NEXT:     }

// The wrappers: same name and signature as the impl method but `async`,
// fresh oneshot reply channel, self.call with the shorthand payload fields
// and the IMMEDIATE await.
// CHECK-NEXT:     async fn bump(&mut self) -> i32 {
// CHECK-NEXT:         let (rtx, rrx) = tokio::sync::oneshot::channel();
// CHECK-NEXT:         self.call(CounterActorMsg::Bump { reply: rtx }, rrx).await
// CHECK-NEXT:     }
// CHECK-NEXT:     async fn set_table(&mut self, i: i32, v: i32) {
// CHECK-NEXT:         let (rtx, rrx) = tokio::sync::oneshot::channel();
// CHECK-NEXT:         self.call(CounterActorMsg::SetTable { i, v, reply: rtx }, rrx).await
// CHECK-NEXT:     }
// CHECK-NEXT: }
emitrust.actor_runtime @CounterActor mode = async

// The driver: a function whose body method_calls an async handle renders
// `async fn`; the handle binding keeps `let mut` (anchor-driven mutability
// classification, both flavors); the spawn construction stays the
// synchronous deferred-init shape (tokio::task::spawn only needs the
// runtime CONTEXT, not an async caller); and every handle method_call —
// the wrapper call and the consuming shutdown alike — appends `.await`.
// CHECK:      async fn c_main() -> i32 {
// CHECK:          let mut counter_actor: CounterActorHandle;
// CHECK:          counter_actor = v{{[0-9]+}};
// CHECK:          counter_actor.bump().await
// CHECK:          counter_actor.shutdown().await;
emitrust.func @c_main() -> i32 {
  %st = emitrust.call_opaque "CounterActor::default"() : () -> !emitrust.struct<"CounterActor">
  %h = emitrust.variable named "counter_actor" : !emitrust.lvalue<!emitrust.opaque<"CounterActorHandle">>
  %sp = emitrust.call_opaque "CounterActorHandle::spawn"(%st) : (!emitrust.struct<"CounterActor">) -> !emitrust.opaque<"CounterActorHandle">
  emitrust.assign %h = %sp : !emitrust.lvalue<!emitrust.opaque<"CounterActorHandle">>
  %0 = emitrust.method_call %h["bump"] () : (!emitrust.lvalue<!emitrust.opaque<"CounterActorHandle">>) -> i32
  emitrust.method_call %h["shutdown"] () : (!emitrust.lvalue<!emitrust.opaque<"CounterActorHandle">>) -> ()
  emitrust.return %0 : i32
}

// A function with no async-handle call stays a plain fn even in a module
// with async anchors.
// CHECK:      fn plain() -> i32 {
emitrust.func @plain() -> i32 {
  %0 = emitrust.constant <7 : i32> : i32
  emitrust.return %0 : i32
}

// The fixed shared runtime, tokio flavor, exactly once, after every module
// item; no second `mod actor_rt` may follow it. reap awaits the
// JoinHandle and re-raises the actor's own panic payload via into_panic —
// the single-panic-provenance contract of the threaded flavor, kept.
// CHECK:      mod actor_rt {
// CHECK-NEXT:     pub struct Handle<M> {
// CHECK-NEXT:         pub tx: tokio::sync::mpsc::UnboundedSender<M>,
// CHECK-NEXT:         pub join: Option<tokio::task::JoinHandle<()>>,
// CHECK-NEXT:     }
// CHECK:          pub async fn reap(&mut self) -> ! {
// CHECK:                  Err(err) if err.is_panic() => std::panic::resume_unwind(err.into_panic()),
// CHECK:          pub async fn call<T>(&mut self, msg: M, rrx: tokio::sync::oneshot::Receiver<T>) -> T {
// CHECK:          pub async fn shutdown(self) {
// CHECK-NOT:  mod actor_rt
