// FR-62 slice 5c: pins the ASYNC crate flavor's manifest delta and its
// exact bounds (E4's posture: a SEPARATELY EMITTED crate flavor, never a
// cargo feature — declaring even a default-off optional dependency breaks
// `cargo build --offline`, the measured NO-GO). Under --actor-mode=async
// --emit=crate the manifest is the default manifest byte-for-byte PLUS one
// appended [dependencies] table carrying the unconditional tokio entry;
// the feature list is the measured minimum ["rt", "sync"] — no "macros",
// because the crate root's main shim is the explicit current_thread
// Builder, not the #[tokio::main] attribute macro. The root pins the async
// synthesis end to end: async fn c_main, the .await at every handle call
// site, the tokio flavor of the runtime, and the block_on shim. The
// DEFAULT manifest must never change: the same program emitted without
// --actor-mode carries no [dependencies] and no tokio, byte-identical
// package/lints tables — pinned by diffing the two manifests' shared
// prefix. And the flavor tracks the MODULE, not the bare flag: a program
// with no actors at all under --actor-mode=async keeps the default
// manifest and the synchronous fn main (nothing references tokio, so the
// offline contract holds).
//
// RUN: emitrust-cc --actor-mode=async --emit=crate %s -o %t.crate
// RUN: cat %t.crate/Cargo.toml | FileCheck %s --check-prefix=TOML
// RUN: cat %t.crate/src/main.rs | FileCheck %s --check-prefix=MAIN
//
// TOML:      [package]
// TOML-NEXT: name = "actor_mode_async_crate"
// TOML-NEXT: version = "0.1.0"
// TOML-NEXT: edition = "2021"
// TOML:      [lints.rust]
// TOML-NEXT: unused_variables = "deny"
// TOML-NEXT: unused_assignments = "deny"
// TOML-NEXT: unused_mut = "deny"
// TOML-NEXT: unused_parens = "deny"
// TOML-NEXT: unpredictable_function_pointer_comparisons = "deny"
// TOML-NEXT: non_snake_case = "deny"
// TOML-NEXT: non_upper_case_globals = "deny"
// TOML-NEXT: non_camel_case_types = "deny"
// TOML-EMPTY:
// TOML-NEXT: [dependencies]
// TOML-NEXT: tokio = { version = "1", features = ["rt", "sync"] }
//
// MAIN:      enum CounterActorMsg {
// MAIN:      reply: tokio::sync::oneshot::Sender<i32> },
// MAIN:      let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<CounterActorMsg>();
// MAIN:      let join = tokio::task::spawn(async move {
// MAIN:      async fn bump(&mut self) -> i32 {
// MAIN:      async fn c_main() -> i32 {
// MAIN:      .bump().await
// MAIN:      .shutdown().await;
// MAIN:      mod actor_rt {
// MAIN:      pub tx: tokio::sync::mpsc::UnboundedSender<M>,
// MAIN:      fn main() { std::process::exit(tokio::runtime::Builder::new_current_thread().enable_all().build().expect("tokio runtime build failed").block_on(c_main())); }
//
// The DEFAULT manifest is untouched by this slice: same program, no
// --actor-mode — the manifest has no [dependencies] and no tokio, and its
// package/lints tables are byte-identical to the async manifest's prefix.
// RUN: emitrust-cc --emit=crate %s -o %t.default.crate
// RUN: not grep -q "dependencies" %t.default.crate/Cargo.toml
// RUN: not grep -q "tokio" %t.default.crate/Cargo.toml
// RUN: head -n $(wc -l < %t.default.crate/Cargo.toml) %t.crate/Cargo.toml > %t.prefix.toml
// RUN: diff %t.default.crate/Cargo.toml %t.prefix.toml
//
// The flavor tracks the module: no actors -> default manifest + sync main
// even under --actor-mode=async.
// RUN: emitrust-cc --actor-mode=async --emit=crate %S/Inputs/no-actors.c -o %t.noactor.crate
// RUN: not grep -q "tokio" %t.noactor.crate/Cargo.toml
// RUN: grep -q "fn main() { std::process::exit(c_main()); }" %t.noactor.crate/src/main.rs

int counter;

int bump(void) {
  counter += 1;
  return counter;
}

int main(void) { return bump(); }
