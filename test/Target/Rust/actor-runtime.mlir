// FR-62 slice 5b: Rust rendering of the `emitrust.actor_runtime` anchor.
// Everything is DERIVED from the referenced struct's impl (the B-prime
// spike verdict — no reified message enum in IR): the <Actor>Msg enum gets
// one variant per method (UpperCamel spelling) whose fields are the
// method's named parameters plus the per-call typed reply channel — a void
// method carries `Sender<()>` so its call stays synchronous under the same
// rule; the <Actor>Handle alias names `actor_rt::Handle<Msg>`; `spawn`
// moves the state onto a std::thread behind an mpsc mailbox with one match
// arm per variant delegating to the existing method and replying; and each
// method gets a same-signature `&mut self` wrapper (fresh reply channel +
// self.call). The shared `mod actor_rt` runtime is a fixed epilogue
// appended ONCE per module however many anchors exist, and the
// opaque-receiver mutability classifier resolves handle method calls
// through the anchors, so the driver's handle binding renders `let mut`
// (without the anchor lookup the closed STL name list would drop the
// `mut` — E0596, found by the spike).
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
// params; the reply channel is the trailing field, typed by the result
// (`Sender<()>` for the void set_table — the unit-reply arm).
// CHECK:      enum CounterActorMsg {
// CHECK-NEXT:     Bump { reply: std::sync::mpsc::Sender<i32> },
// CHECK-NEXT:     SetTable { i: i32, v: i32, reply: std::sync::mpsc::Sender<()> },
// CHECK-NEXT: }
// CHECK-NEXT: type CounterActorHandle = actor_rt::Handle<CounterActorMsg>;

// The spawn mailbox loop: moved state, one arm per variant, delegation +
// reply under the fixed expect message.
// CHECK-NEXT: impl actor_rt::Handle<CounterActorMsg> {
// CHECK-NEXT:     fn spawn(mut state: CounterActor) -> Self {
// CHECK-NEXT:         let (tx, rx) = std::sync::mpsc::channel::<CounterActorMsg>();
// CHECK-NEXT:         let join = std::thread::spawn(move || {
// CHECK-NEXT:             for msg in rx {
// CHECK-NEXT:                 match msg {
// CHECK-NEXT:                     CounterActorMsg::Bump { reply } => {
// CHECK-NEXT:                         reply.send(state.bump()).expect("actor caller dropped reply receiver");
// CHECK-NEXT:                     }
// CHECK-NEXT:                     CounterActorMsg::SetTable { i, v, reply } => {
// CHECK-NEXT:                         reply.send(state.set_table(i, v)).expect("actor caller dropped reply receiver");
// CHECK-NEXT:                     }
// CHECK-NEXT:                 }
// CHECK-NEXT:             }
// CHECK-NEXT:         });
// CHECK-NEXT:         Self { tx, join: Some(join) }
// CHECK-NEXT:     }

// The wrappers: same name and signature as the impl method, fresh reply
// channel, self.call with the shorthand payload fields.
// CHECK-NEXT:     fn bump(&mut self) -> i32 {
// CHECK-NEXT:         let (rtx, rrx) = std::sync::mpsc::channel();
// CHECK-NEXT:         self.call(CounterActorMsg::Bump { reply: rtx }, rrx)
// CHECK-NEXT:     }
// CHECK-NEXT:     fn set_table(&mut self, i: i32, v: i32) {
// CHECK-NEXT:         let (rtx, rrx) = std::sync::mpsc::channel();
// CHECK-NEXT:         self.call(CounterActorMsg::SetTable { i, v, reply: rtx }, rrx)
// CHECK-NEXT:     }
// CHECK-NEXT: }
emitrust.actor_runtime @CounterActor mode = threaded

emitrust.struct_def @PingActor ["hits"] [i32]
emitrust.impl "PingActor" {
  emitrust.func @ping(%arg0: !emitrust.mut_ref<!emitrust.struct<"PingActor">>) {
    %0 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"PingActor">>) -> !emitrust.lvalue<!emitrust.struct<"PingActor">>
    %1 = emitrust.member %0["hits"] : (!emitrust.lvalue<!emitrust.struct<"PingActor">>) -> !emitrust.lvalue<i32>
    %2 = emitrust.constant <1 : i32> : i32
    emitrust.assign %1 = %2 : !emitrust.lvalue<i32>
    emitrust.return
  }
}
// The second actor gets its own enum/alias/impl; the shared runtime does
// NOT repeat per anchor.
// CHECK:      enum PingActorMsg {
// CHECK-NEXT:     Ping { reply: std::sync::mpsc::Sender<()> },
// CHECK-NEXT: }
// CHECK-NEXT: type PingActorHandle = actor_rt::Handle<PingActorMsg>;
// CHECK:      fn ping(&mut self) {
emitrust.actor_runtime @PingActor mode = threaded

// The driver: the handle binding must render `let mut` (the anchor-driven
// mutability classification), the spawn construction stays the spike's
// deferred-init shape, and shutdown renders as a plain method call.
// CHECK:      fn c_main() -> i32 {
// CHECK:          let v{{[0-9]+}}: CounterActorHandle = CounterActorHandle::spawn(
// CHECK:          let mut counter_actor: CounterActorHandle = v{{[0-9]+}};
// CHECK:          counter_actor.bump()
// CHECK:          counter_actor.shutdown();
emitrust.func @c_main() -> i32 {
  %st = emitrust.call_opaque "CounterActor::default"() : () -> !emitrust.struct<"CounterActor">
  %h = emitrust.variable named "counter_actor" : !emitrust.lvalue<!emitrust.opaque<"CounterActorHandle">>
  %sp = emitrust.call_opaque "CounterActorHandle::spawn"(%st) : (!emitrust.struct<"CounterActor">) -> !emitrust.opaque<"CounterActorHandle">
  emitrust.assign %h = %sp : !emitrust.lvalue<!emitrust.opaque<"CounterActorHandle">>
  %0 = emitrust.method_call %h["bump"] () : (!emitrust.lvalue<!emitrust.opaque<"CounterActorHandle">>) -> i32
  emitrust.method_call %h["shutdown"] () : (!emitrust.lvalue<!emitrust.opaque<"CounterActorHandle">>) -> ()
  emitrust.return %0 : i32
}

// The fixed shared runtime, exactly once, after every module item; no
// second `mod actor_rt` may follow it.
// CHECK:      mod actor_rt {
// CHECK-NEXT:     pub struct Handle<M> {
// CHECK-NEXT:         pub tx: std::sync::mpsc::Sender<M>,
// CHECK-NEXT:         pub join: Option<std::thread::JoinHandle<()>>,
// CHECK-NEXT:     }
// CHECK:          pub fn reap(&mut self) -> ! {
// CHECK:          pub fn call<T>(&mut self, msg: M, rrx: std::sync::mpsc::Receiver<T>) -> T {
// CHECK:          pub fn shutdown(self) {
// CHECK-NOT:  mod actor_rt
