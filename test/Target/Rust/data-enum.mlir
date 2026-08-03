// FR-62 slice 5a: Rust rendering of the closed data-enum surface.
// emitrust.data_enum_def renders a REAL Rust enum (contrast the open
// emitrust.enum_def tuple-struct rendering pinned in match.mlir):
// #[derive(Clone, Copy)] -- deliberately no Default (a closed enum has no
// canonical default value, unlike struct_def's unconditional Default) and
// no PartialEq (a struct payload field derives none) -- with UpperCamel
// variants, unit variants bare and data variants with named snake_case
// fields. emitrust.enum_variant renders `Name::Variant { field: value }`
// (`Name::Variant` for a unit variant) and participates in the FR-61a
// tail fold like every let-producing expression op. emitrust.match
// renders an EXHAUSTIVE Rust match with variant patterns -- one arm per
// variant, no `_` arm (contrast emitrust.switch's mandatory default) --
// binding payload fields to block-argument names; a never-read binding
// gets the `_` prefix so the deny-manifest unused_variables lint stays
// clean. Result mode renders as a match expression: `let v = match ...`
// in binding position and a bare tail expression under the FR-61a fold.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK:      #[derive(Clone, Copy)]
// CHECK-NEXT: enum Signal {
// CHECK-NEXT:     Go,
// CHECK-NEXT:     Stop,
// CHECK-NEXT: }
emitrust.data_enum_def @Signal ["Go", "Stop"] [[], []] [[], []]

// CHECK:      #[derive(Clone, Copy)]
// CHECK-NEXT: enum Msg {
// CHECK-NEXT:     Quit,
// CHECK-NEXT:     Add { amount: i32 },
// CHECK-NEXT:     Move { x: i32, y: i32 },
// CHECK-NEXT: }
emitrust.data_enum_def @Msg ["Quit", "Add", "Move"] [[], ["amount"], ["x", "y"]] [[], [i32], [i32, i32]]

// Construction in let position (the single-use constant inlines as a
// suffixed literal) and the constructed value in call-arg position.
// CHECK-LABEL: fn send() {
// CHECK-NEXT:    let v1: Msg = Msg::Add { amount: 5i32 };
// CHECK-NEXT:    deliver(v1);
// CHECK-NEXT:  }
emitrust.func @send() {
  %five = emitrust.constant <5 : i32> : i32
  %m = emitrust.enum_variant @Msg "Add"(%five) : (i32) -> !emitrust.data_enum<"Msg">
  emitrust.call_opaque "deliver"(%m) : (!emitrust.data_enum<"Msg">) -> ()
  emitrust.return
}

// A unit variant renders bare; the function-final construction folds into
// the tail expression (FR-61a integration of the construction op).
// CHECK-LABEL: fn make_quit() -> Msg {
// CHECK-NEXT:    Msg::Quit
// CHECK-NEXT:  }
emitrust.func @make_quit() -> !emitrust.data_enum<"Msg"> {
  %m = emitrust.enum_variant @Msg "Quit"() : () -> !emitrust.data_enum<"Msg">
  emitrust.return %m : !emitrust.data_enum<"Msg">
}

// CHECK-LABEL: fn make_move(v0: i32) -> Msg {
// CHECK-NEXT:    Msg::Move { x: v0, y: v0 }
// CHECK-NEXT:  }
emitrust.func @make_move(%arg0: i32) -> !emitrust.data_enum<"Msg"> {
  %m = emitrust.enum_variant @Msg "Move"(%arg0, %arg0) : (i32, i32) -> !emitrust.data_enum<"Msg">
  emitrust.return %m : !emitrust.data_enum<"Msg">
}

// Statement mode: variant patterns with field bindings, no default arm
// (the match is exhaustive by construction). The unread `y` binding
// renders `_`-prefixed so unused_variables stays clean.
// CHECK-LABEL: fn dispatch(v0: Msg) {
// CHECK-NEXT:    match v0 {
// CHECK-NEXT:        Msg::Quit => {
// CHECK-NEXT:        }
// CHECK-NEXT:        Msg::Add { amount: v1 } => {
// CHECK-NEXT:            on_add(v1);
// CHECK-NEXT:        }
// CHECK-NEXT:        Msg::Move { x: v2, y: _v3 } => {
// CHECK-NEXT:            on_move(v2);
// CHECK-NEXT:        }
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @dispatch(%arg0: !emitrust.data_enum<"Msg">) {
  emitrust.match %arg0 : !emitrust.data_enum<"Msg">
  case "Quit" {
  }
  case "Add" (%amount : i32) {
    emitrust.call_opaque "on_add"(%amount) : (i32) -> ()
  }
  case "Move" (%x : i32, %y : i32) {
    emitrust.call_opaque "on_move"(%x) : (i32) -> ()
  }
  emitrust.return
}

// Result mode in binding position: the match renders as the right-hand
// side of the let; each arm's yield operand is the arm's tail expression
// (arm-local pure producers inline into it). The result is read twice, so
// the binding survives, and the final add folds into the function tail.
// CHECK-LABEL: fn weigh(v0: Msg) -> i32 {
// CHECK-NEXT:    let v1: i32 = match v0 {
// CHECK-NEXT:        Msg::Quit => {
// CHECK-NEXT:            0i32
// CHECK-NEXT:        }
// CHECK-NEXT:        Msg::Add { amount: v3 } => {
// CHECK-NEXT:            v3
// CHECK-NEXT:        }
// CHECK-NEXT:        Msg::Move { x: v4, y: v5 } => {
// CHECK-NEXT:            v4 + v5
// CHECK-NEXT:        }
// CHECK-NEXT:    };
// CHECK-NEXT:    v1 + v1
// CHECK-NEXT:  }
emitrust.func @weigh(%arg0: !emitrust.data_enum<"Msg">) -> i32 {
  %r = emitrust.match %arg0 : !emitrust.data_enum<"Msg"> -> i32
  case "Quit" {
    %c0 = emitrust.constant <0 : i32> : i32
    emitrust.yield %c0 : i32
  }
  case "Add" (%amount : i32) {
    emitrust.yield %amount : i32
  }
  case "Move" (%x : i32, %y : i32) {
    %s = emitrust.add %x, %y : i32
    emitrust.yield %s : i32
  }
  %dbl = emitrust.add %r, %r : i32
  emitrust.return %dbl : i32
}

// Result mode as the function tail: the FR-61a fold suppresses the
// binding and the bare match expression is the function body's tail.
// CHECK-LABEL: fn signal_code(v0: Signal) -> i32 {
// CHECK-NEXT:    match v0 {
// CHECK-NEXT:        Signal::Go => {
// CHECK-NEXT:            1i32
// CHECK-NEXT:        }
// CHECK-NEXT:        Signal::Stop => {
// CHECK-NEXT:            0i32
// CHECK-NEXT:        }
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @signal_code(%arg0: !emitrust.data_enum<"Signal">) -> i32 {
  %r = emitrust.match %arg0 : !emitrust.data_enum<"Signal"> -> i32
  case "Go" {
    %c1 = emitrust.constant <1 : i32> : i32
    emitrust.yield %c1 : i32
  }
  case "Stop" {
    %c0 = emitrust.constant <0 : i32> : i32
    emitrust.yield %c0 : i32
  }
  emitrust.return %r : i32
}
