// FR-62 slice 5a: the closed data-enum surface round-trips losslessly.
// This pins the parse/print contract of the three new ops:
// emitrust.data_enum_def (unit-only, single-field, multi-field, and nested
// struct/array/open-enum payload variants), emitrust.enum_variant
// construction (unit and data variants), and emitrust.match in both its
// statement mode (no result, implicit hidden terminators) and its result
// mode (`-> T`, value-carrying emitrust.yield printed explicitly). The
// custom match syntax must reprint byte-stably through a second
// emitrust-opt, exactly like emitrust.switch does.
// RUN: emitrust-opt %s | emitrust-opt | FileCheck %s

// CHECK: emitrust.data_enum_def @Signal ["Go", "Stop"] {{\[\[}}], {{\[}}]] {{\[\[}}], {{\[}}]]
emitrust.data_enum_def @Signal ["Go", "Stop"] [[], []] [[], []]

// CHECK: emitrust.data_enum_def @Msg ["Quit", "Add", "Move"] {{\[\[}}], {{\[}}"amount"], {{\[}}"x", "y"]] {{\[\[}}], {{\[}}i32], {{\[}}i32, i32]]
emitrust.data_enum_def @Msg ["Quit", "Add", "Move"] [[], ["amount"], ["x", "y"]] [[], [i32], [i32, i32]]

// A variant may carry the full struct_def-permitted payload set: nested
// struct, array, and OPEN enum fields.
// CHECK: emitrust.data_enum_def @Payload ["Pt", "Buf", "Col"] {{\[\[}}"p"], {{\[}}"data"], {{\[}}"c"]] {{\[\[}}!emitrust.struct<"Point">], {{\[}}!emitrust.array<4xi32>], {{\[}}!emitrust.enum<"Color">]]
emitrust.data_enum_def @Payload ["Pt", "Buf", "Col"] [["p"], ["data"], ["c"]]
    [[!emitrust.struct<"Point">], [!emitrust.array<4xi32>], [!emitrust.enum<"Color">]]

// CHECK-LABEL: emitrust.func @construct
emitrust.func @construct(%arg0: i32) -> !emitrust.data_enum<"Msg"> {
  // CHECK: emitrust.enum_variant @Msg "Quit"() : () -> !emitrust.data_enum<"Msg">
  %q = emitrust.enum_variant @Msg "Quit"() : () -> !emitrust.data_enum<"Msg">
  // CHECK: emitrust.enum_variant @Msg "Add"(%{{.*}}) : (i32) -> !emitrust.data_enum<"Msg">
  %a = emitrust.enum_variant @Msg "Add"(%arg0) : (i32) -> !emitrust.data_enum<"Msg">
  // CHECK: emitrust.enum_variant @Msg "Move"(%{{.*}}, %{{.*}}) : (i32, i32) -> !emitrust.data_enum<"Msg">
  %m = emitrust.enum_variant @Msg "Move"(%arg0, %arg0) : (i32, i32) -> !emitrust.data_enum<"Msg">
  emitrust.return %m : !emitrust.data_enum<"Msg">
}

// Statement mode: no result, one case per variant in declaration order,
// data cases binding their payload fields as block arguments. The implicit
// empty terminators stay hidden on reprint.
// CHECK-LABEL: emitrust.func @match_statement
emitrust.func @match_statement(%arg0: !emitrust.data_enum<"Msg">) {
  // CHECK: emitrust.match %{{.*}} : !emitrust.data_enum<"Msg">
  // CHECK-NEXT: case "Quit" {
  // CHECK-NOT: emitrust.yield
  // CHECK: case "Add" (%{{.*}}: i32) {
  // CHECK: case "Move" (%{{.*}}: i32, %{{.*}}: i32) {
  emitrust.match %arg0 : !emitrust.data_enum<"Msg">
  case "Quit" {
  }
  case "Add" (%amount : i32) {
    %0 = emitrust.let %amount : i32
  }
  case "Move" (%x : i32, %y : i32) {
    %1 = emitrust.let %x : i32
  }
  emitrust.return
}

// Result mode: `-> i32` after the scrutinee type, every case terminated by
// a value-carrying emitrust.yield that must survive the round-trip.
// CHECK-LABEL: emitrust.func @match_result
emitrust.func @match_result(%arg0: !emitrust.data_enum<"Msg">) -> i32 {
  // CHECK: %{{.*}} = emitrust.match %{{.*}} : !emitrust.data_enum<"Msg"> -> i32
  // CHECK-NEXT: case "Quit" {
  // CHECK: emitrust.yield %{{.*}} : i32
  // CHECK: case "Add" (%{{.*}}: i32) {
  // CHECK: case "Move" (%{{.*}}: i32, %{{.*}}: i32) {
  %r = emitrust.match %arg0 : !emitrust.data_enum<"Msg"> -> i32
  case "Quit" {
    %c = emitrust.constant <0 : i32> : i32
    emitrust.yield %c : i32
  }
  case "Add" (%amount : i32) {
    emitrust.yield %amount : i32
  }
  case "Move" (%x : i32, %y : i32) {
    %s = emitrust.add %x, %y : i32
    emitrust.yield %s : i32
  }
  emitrust.return %r : i32
}

// A unit-only enum matches with argument-less cases in both modes.
// CHECK-LABEL: emitrust.func @match_unit_only
emitrust.func @match_unit_only(%arg0: !emitrust.data_enum<"Signal">) -> i32 {
  // CHECK: emitrust.match %{{.*}} : !emitrust.data_enum<"Signal"> -> i32
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
