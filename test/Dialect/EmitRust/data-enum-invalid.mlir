// FR-62 slice 5a: the closed data-enum verifiers reject malformed IR with
// located diagnostics. This pins the whole rejection surface: def-level
// shape errors (array-length mismatches, duplicate/empty names, invalid
// payload types), construction errors (unresolved or wrong-kind symbol,
// unknown variant, arity and operand-type mismatches, result-type
// mismatch), match errors (non-data_enum scrutinee, unresolved def,
// non-exhaustive or out-of-order cases -- a closed-enum match has NO
// default arm, coverage is total by construction -- wrong block-argument
// count/types, and both yield-mode violations), and the yield operand
// gate outside emitrust.match. Rejection is a feature: none of these may
// ever reach emission.
// RUN: emitrust-opt %s --split-input-file --verify-diagnostics

// expected-error @+1 {{must have at least one variant}}
emitrust.data_enum_def @E [] [] []

// -----

// expected-error @+1 {{has 2 variant names but 1 variant field-name lists}}
emitrust.data_enum_def @E ["A", "B"] [[]] [[], []]

// -----

// expected-error @+1 {{has 2 variant names but 1 variant field-type lists}}
emitrust.data_enum_def @E ["A", "B"] [[], []] [[]]

// -----

// expected-error @+1 {{duplicate variant name "A"}}
emitrust.data_enum_def @E ["A", "A"] [[], []] [[], []]

// -----

// expected-error @+1 {{variant names must not be empty}}
emitrust.data_enum_def @E [""] [[]] [[]]

// -----

// expected-error @+1 {{variant "A" has 2 field names but 1 field types}}
emitrust.data_enum_def @E ["A"] [["x", "y"]] [[i32]]

// -----

// expected-error @+1 {{variant "A" field names must not be empty}}
emitrust.data_enum_def @E ["A"] [[""]] [[i32]]

// -----

// expected-error @+1 {{duplicate field name "x" in variant "A"}}
emitrust.data_enum_def @E ["A"] [["x", "x"]] [[i32, i32]]

// -----

// expected-error @+1 {{variant "A" field names must be an array of strings}}
emitrust.data_enum_def @E ["A"] [[3]] [[i32]]

// -----

// expected-error @+1 {{invalid field type '!emitrust.lvalue<i32>' in variant "A"}}
emitrust.data_enum_def @E ["A"] [["x"]] [[!emitrust.lvalue<i32>]]

// -----

emitrust.func @no_def() {
  // expected-error @+1 {{references '@Missing' which is not a visible emitrust.data_enum_def}}
  %m = emitrust.enum_variant @Missing "A"() : () -> !emitrust.data_enum<"Missing">
  emitrust.return
}

// -----

// An OPEN emitrust.enum_def is a different animal: constructing it through
// the closed-enum construction op is rejected as a wrong-kind reference.
emitrust.enum_def @Color ["Red"] [0]
emitrust.func @open_enum() {
  // expected-error @+1 {{references '@Color' which is not a visible emitrust.data_enum_def}}
  %m = emitrust.enum_variant @Color "Red"() : () -> !emitrust.data_enum<"Color">
  emitrust.return
}

// -----

emitrust.data_enum_def @Msg ["Quit", "Add"] [[], ["amount"]] [[], [i32]]
emitrust.func @unknown_variant() {
  // expected-error @+1 {{references unknown variant "Bad" of '@Msg'}}
  %m = emitrust.enum_variant @Msg "Bad"() : () -> !emitrust.data_enum<"Msg">
  emitrust.return
}

// -----

emitrust.data_enum_def @Msg ["Quit", "Add"] [[], ["amount"]] [[], [i32]]
emitrust.func @wrong_arity() {
  // expected-error @+1 {{variant "Add" has 1 fields, but the construction supplies 0 operands}}
  %m = emitrust.enum_variant @Msg "Add"() : () -> !emitrust.data_enum<"Msg">
  emitrust.return
}

// -----

emitrust.data_enum_def @Msg ["Quit", "Add"] [[], ["amount"]] [[], [i32]]
emitrust.func @wrong_operand_type(%arg0: i64) {
  // expected-error @+1 {{operand #0 has type 'i64', but field "amount" of variant "Add" has type 'i32'}}
  %m = emitrust.enum_variant @Msg "Add"(%arg0) : (i64) -> !emitrust.data_enum<"Msg">
  emitrust.return
}

// -----

emitrust.data_enum_def @Msg ["Quit"] [[]] [[]]
emitrust.data_enum_def @Other ["Quit"] [[]] [[]]
emitrust.func @wrong_result_enum() {
  // expected-error @+1 {{result type '!emitrust.data_enum<"Other">' does not reference enum '@Msg'}}
  %m = emitrust.enum_variant @Msg "Quit"() : () -> !emitrust.data_enum<"Other">
  emitrust.return
}

// -----

emitrust.func @int_scrutinee(%arg0: i32) {
  // expected-error @+1 {{must be EmitRust data enum type}}
  emitrust.match %arg0 : i32
  case "A" {
  }
  emitrust.return
}

// -----

emitrust.func @match_no_def(%arg0: !emitrust.data_enum<"Missing">) {
  // expected-error @+1 {{scrutinee type '!emitrust.data_enum<"Missing">' requires a visible emitrust.data_enum_def}}
  emitrust.match %arg0 : !emitrust.data_enum<"Missing">
  case "A" {
  }
  emitrust.return
}

// -----

emitrust.data_enum_def @Msg ["Quit", "Add"] [[], ["amount"]] [[], [i32]]
emitrust.func @non_exhaustive(%arg0: !emitrust.data_enum<"Msg">) {
  // expected-error @+1 {{has 1 cases but 'Msg' declares 2 variants (a closed-enum match is exhaustive and has no default arm)}}
  emitrust.match %arg0 : !emitrust.data_enum<"Msg">
  case "Quit" {
  }
  emitrust.return
}

// -----

emitrust.data_enum_def @Msg ["Quit", "Add"] [[], ["amount"]] [[], [i32]]
emitrust.func @wrong_order(%arg0: !emitrust.data_enum<"Msg">) {
  // expected-error @+1 {{case #0 is "Add" but 'Msg' declares "Quit" here (cases follow declaration order)}}
  emitrust.match %arg0 : !emitrust.data_enum<"Msg">
  case "Add" (%amount : i32) {
  }
  case "Quit" {
  }
  emitrust.return
}

// -----

emitrust.data_enum_def @Msg ["Quit", "Add"] [[], ["amount"]] [[], [i32]]
emitrust.func @missing_binding(%arg0: !emitrust.data_enum<"Msg">) {
  // expected-error @+1 {{case "Add" region must have 1 block arguments (one per variant field), but has 0}}
  emitrust.match %arg0 : !emitrust.data_enum<"Msg">
  case "Quit" {
  }
  case "Add" {
  }
  emitrust.return
}

// -----

emitrust.data_enum_def @Msg ["Quit", "Add"] [[], ["amount"]] [[], [i32]]
emitrust.func @wrong_binding_type(%arg0: !emitrust.data_enum<"Msg">) {
  // expected-error @+1 {{case "Add" block argument #0 has type 'i64', but field "amount" has type 'i32'}}
  emitrust.match %arg0 : !emitrust.data_enum<"Msg">
  case "Quit" {
  }
  case "Add" (%amount : i64) {
  }
  emitrust.return
}

// -----

emitrust.data_enum_def @Msg ["Quit"] [[]] [[]]
emitrust.func @statement_yields_value(%arg0: !emitrust.data_enum<"Msg">, %arg1: i32) {
  // expected-error @+1 {{case "Quit" must not yield a value on a match without a result}}
  emitrust.match %arg0 : !emitrust.data_enum<"Msg">
  case "Quit" {
    emitrust.yield %arg1 : i32
  }
  emitrust.return
}

// -----

emitrust.data_enum_def @Msg ["Quit"] [[]] [[]]
emitrust.func @result_without_yield(%arg0: !emitrust.data_enum<"Msg">) -> i32 {
  // expected-error @+1 {{case "Quit" must yield exactly one value of the match result type}}
  %r = emitrust.match %arg0 : !emitrust.data_enum<"Msg"> -> i32
  case "Quit" {
  }
  emitrust.return %r : i32
}

// -----

emitrust.data_enum_def @Msg ["Quit"] [[]] [[]]
emitrust.func @result_yield_type(%arg0: !emitrust.data_enum<"Msg">, %arg1: i64) -> i32 {
  // expected-error @+1 {{case "Quit" yields 'i64' but the match result type is 'i32'}}
  %r = emitrust.match %arg0 : !emitrust.data_enum<"Msg"> -> i32
  case "Quit" {
    emitrust.yield %arg1 : i64
  }
  emitrust.return %r : i32
}

// -----

emitrust.func @yield_outside_match(%arg0: i1, %arg1: i32) {
  emitrust.if %arg0 {
    // expected-error @+1 {{operands are only supported inside an emitrust.match}}
    emitrust.yield %arg1 : i32
  }
  emitrust.return
}
