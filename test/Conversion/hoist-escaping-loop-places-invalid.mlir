// FR-152: the FENCE on the escaping-loop-place hoist, at IR level.
//
// The hoist stops recreating the place per iteration. For a type whose Rust
// rendering has a `Drop` that silently deletes every drop but the last -- a
// compile-clean miscompile the byte-diff oracle caught and `cargo build` never
// could. So a place whose lvalue value type transitively reaches a
// `emitrust.struct_def` carrying `emitrust.has_drop`, or is an
// `emitrust.opaque` (a `String`/`Vec` this importer produces always owns a
// heap allocation), is REJECTED rather than hoisted.
//
// Rejection is a feature: it is a located error at the declaration, not a
// silent skip that would fall through to the pipeline's much later and much
// less useful `failed to legalize operation 'scf.while'`.
//
// The transitive leg is what `@wrapped` pins: `W` has no `has_drop` of its
// own, it merely HOLDS an `R` that does. A fence that looked only at the
// outermost type name would admit it and delete `R`'s drops.
//
// RUN: not emitrust-opt %s --emitrust-hoist-escaping-loop-places \
// RUN:   --split-input-file 2>&1 | FileCheck %s

emitrust.struct_def @R ["id"] [i32] {emitrust.has_drop}

// CHECK: error: unsupported: local 'r' has a destructor, is declared inside a loop body, and escapes the loop
// CHECK: note: moving the declaration out of the loop would delete every per-iteration drop but the last
func.func @direct(%arg0: i32) -> i32 {
  %c1_i32 = arith.constant 1 : i32
  %c0_i32 = arith.constant 0 : i32
  %0:2 = scf.while (%arg1 = %arg0) : (i32) -> (i32, !emitrust.lvalue<!emitrust.struct<"R">>) {
    %3 = emitrust.variable named "r" : !emitrust.lvalue<!emitrust.struct<"R">>
    %4 = emitrust.member %3["id"] : (!emitrust.lvalue<!emitrust.struct<"R">>) -> !emitrust.lvalue<i32>
    emitrust.assign %4 = %arg1 : !emitrust.lvalue<i32>
    %5 = arith.addi %arg1, %c1_i32 : i32
    %6 = arith.cmpi ne, %arg1, %c0_i32 : i32
    scf.condition(%6) %5, %3 : i32, !emitrust.lvalue<!emitrust.struct<"R">>
  } do {
  ^bb0(%arg1: i32, %arg2: !emitrust.lvalue<!emitrust.struct<"R">>):
    scf.yield %arg1 : i32
  }
  %1 = emitrust.member %0#1["id"] : (!emitrust.lvalue<!emitrust.struct<"R">>) -> !emitrust.lvalue<i32>
  %2 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
  return %2 : i32
}

// -----

emitrust.struct_def @R ["id"] [i32] {emitrust.has_drop}
emitrust.struct_def @W ["r", "k"] [!emitrust.struct<"R">, i32]

// CHECK: error: unsupported: local 'w' has a destructor, is declared inside a loop body, and escapes the loop
func.func @wrapped(%arg0: i32) -> i32 {
  %c1_i32 = arith.constant 1 : i32
  %c0_i32 = arith.constant 0 : i32
  %0:2 = scf.while (%arg1 = %arg0) : (i32) -> (i32, !emitrust.lvalue<!emitrust.struct<"W">>) {
    %3 = emitrust.variable named "w" : !emitrust.lvalue<!emitrust.struct<"W">>
    %4 = emitrust.member %3["k"] : (!emitrust.lvalue<!emitrust.struct<"W">>) -> !emitrust.lvalue<i32>
    emitrust.assign %4 = %arg1 : !emitrust.lvalue<i32>
    %5 = arith.addi %arg1, %c1_i32 : i32
    %6 = arith.cmpi ne, %arg1, %c0_i32 : i32
    scf.condition(%6) %5, %3 : i32, !emitrust.lvalue<!emitrust.struct<"W">>
  } do {
  ^bb0(%arg1: i32, %arg2: !emitrust.lvalue<!emitrust.struct<"W">>):
    scf.yield %arg1 : i32
  }
  %1 = emitrust.member %0#1["k"] : (!emitrust.lvalue<!emitrust.struct<"W">>) -> !emitrust.lvalue<i32>
  %2 = emitrust.load %1 : (!emitrust.lvalue<i32>) -> i32
  return %2 : i32
}

// -----

// CHECK: error: unsupported: local 's' has a destructor, is declared inside a loop body, and escapes the loop
func.func @opaque(%arg0: i32) -> i32 {
  %c1_i32 = arith.constant 1 : i32
  %c0_i32 = arith.constant 0 : i32
  %0:2 = scf.while (%arg1 = %arg0) : (i32) -> (i32, !emitrust.lvalue<!emitrust.opaque<"String">>) {
    %2 = emitrust.variable named "s" : !emitrust.lvalue<!emitrust.opaque<"String">>
    %3 = arith.addi %arg1, %c1_i32 : i32
    %4 = arith.cmpi ne, %arg1, %c0_i32 : i32
    scf.condition(%4) %3, %2 : i32, !emitrust.lvalue<!emitrust.opaque<"String">>
  } do {
  ^bb0(%arg1: i32, %arg2: !emitrust.lvalue<!emitrust.opaque<"String">>):
    scf.yield %arg1 : i32
  }
  %1 = emitrust.load %0#1 : (!emitrust.lvalue<!emitrust.opaque<"String">>) -> !emitrust.opaque<"String">
  return %arg0 : i32
}
