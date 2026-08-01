// FR-39 (W4.2e Part B): emitrust-lower-containers lowers the backend-agnostic
// container ops (emitrust.collection / collection_push / collection_at) to the
// production array backend — a fixed [T;CAP] pool (an emitrust.variable) plus a
// rank-0 memref i64 free cursor. This reproduces the exact legacy node-pool
// shape the importer previously inlined; the pass runs before mem2reg so the
// cursor cell promotes downstream exactly as if the pool were inlined. Input is
// the validated spike kernel (a 5-node singly linked list, val=i prepended,
// summed -> 10).
// RUN: emitrust-opt --emitrust-lower-containers %s | FileCheck %s

module {
  emitrust.struct_def @Node ["val", "next"] [i32, !emitrust.opaque<"Option<usize>">]
  // CHECK-LABEL: func.func @c_main
  func.func @c_main() -> i32 {
    // The collection becomes a [Node; 5] pool + a rank-0 i64 cursor cell
    // initialized to 0. The Pure cursor constants hoist to the entry block.
    // CHECK-DAG: %[[ONE:.*]] = arith.constant 1 : i64
    // CHECK-DAG: %[[ZERO:.*]] = arith.constant 0 : i64
    // CHECK: %[[POOL:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<5x!emitrust.struct<"Node">>>
    // CHECK: %[[CUR:.*]] = memref.alloca() : memref<i64>
    // CHECK: memref.store %[[ZERO]], %[[CUR]][] : memref<i64>
    %col = emitrust.collection {element_type = !emitrust.struct<"Node">, capacity = 5 : i64} : !emitrust.lvalue<!emitrust.opaque<"__emitrust_collection">>
    %h_nn = emitrust.variable : !emitrust.lvalue<i1>
    %h_idx = emitrust.variable : !emitrust.lvalue<i64>

    %c0 = arith.constant 0 : index
    %c5 = arith.constant 5 : index
    %c1 = arith.constant 1 : index
    %true = emitrust.constant <true> : i1

    scf.for %i = %c0 to %c5 step %c1 {
      %iv = arith.index_cast %i : index to i32
      // push: idx = cursor; cursor = cursor + 1.
      // CHECK: %[[IDX:.*]] = memref.load %[[CUR]][] : memref<i64>
      // CHECK: %[[NXT:.*]] = arith.addi %[[IDX]], %[[ONE]] : i64
      // CHECK: memref.store %[[NXT]], %[[CUR]][] : memref<i64>
      %idx = emitrust.collection_push %col : (!emitrust.lvalue<!emitrust.opaque<"__emitrust_collection">>) -> i64
      // at(idx) subscripts the pool.
      // CHECK: emitrust.subscript %[[POOL]][%[[IDX]]] : (!emitrust.lvalue<!emitrust.array<5x!emitrust.struct<"Node">>>, i64) -> !emitrust.lvalue<!emitrust.struct<"Node">>
      %slot = emitrust.collection_at %col[%idx] : (!emitrust.lvalue<!emitrust.opaque<"__emitrust_collection">>, i64) -> !emitrust.lvalue<!emitrust.struct<"Node">>
      %valf = emitrust.member %slot["val"] : (!emitrust.lvalue<!emitrust.struct<"Node">>) -> !emitrust.lvalue<i32>
      emitrust.assign %valf = %iv : !emitrust.lvalue<i32>
      %nn = emitrust.load %h_nn : (!emitrust.lvalue<i1>) -> i1
      %hi = emitrust.load %h_idx : (!emitrust.lvalue<i64>) -> i64
      %opt = emitrust.call_opaque "__emitrust_pool_opt"(%nn, %hi) : (i1, i64) -> !emitrust.opaque<"Option<usize>">
      %nextf = emitrust.member %slot["next"] : (!emitrust.lvalue<!emitrust.struct<"Node">>) -> !emitrust.lvalue<!emitrust.opaque<"Option<usize>">>
      emitrust.assign %nextf = %opt : !emitrust.lvalue<!emitrust.opaque<"Option<usize>">>
      emitrust.assign %h_nn = %true : !emitrust.lvalue<i1>
      emitrust.assign %h_idx = %idx : !emitrust.lvalue<i64>
    }

    %sum = emitrust.variable : !emitrust.lvalue<i32>
    %c_nn = emitrust.variable : !emitrust.lvalue<i1>
    %c_idx = emitrust.variable : !emitrust.lvalue<i64>
    %ihn = emitrust.load %h_nn : (!emitrust.lvalue<i1>) -> i1
    %ihi = emitrust.load %h_idx : (!emitrust.lvalue<i64>) -> i64
    emitrust.assign %c_nn = %ihn : !emitrust.lvalue<i1>
    emitrust.assign %c_idx = %ihi : !emitrust.lvalue<i64>

    scf.while : () -> () {
      %cond = emitrust.load %c_nn : (!emitrust.lvalue<i1>) -> i1
      scf.condition(%cond)
    } do {
      %ci = emitrust.load %c_idx : (!emitrust.lvalue<i64>) -> i64
      %slot2 = emitrust.collection_at %col[%ci] : (!emitrust.lvalue<!emitrust.opaque<"__emitrust_collection">>, i64) -> !emitrust.lvalue<!emitrust.struct<"Node">>
      %valf2 = emitrust.member %slot2["val"] : (!emitrust.lvalue<!emitrust.struct<"Node">>) -> !emitrust.lvalue<i32>
      %v = emitrust.load %valf2 : (!emitrust.lvalue<i32>) -> i32
      %s = emitrust.load %sum : (!emitrust.lvalue<i32>) -> i32
      %ns = emitrust.add %s, %v : i32
      emitrust.assign %sum = %ns : !emitrust.lvalue<i32>
      %nextf2 = emitrust.member %slot2["next"] : (!emitrust.lvalue<!emitrust.struct<"Node">>) -> !emitrust.lvalue<!emitrust.opaque<"Option<usize>">>
      %optv = emitrust.load %nextf2 : (!emitrust.lvalue<!emitrust.opaque<"Option<usize>">>) -> !emitrust.opaque<"Option<usize>">
      %unp:2 = emitrust.call_opaque "__emitrust_pool_unpack"(%optv) : (!emitrust.opaque<"Option<usize>">) -> (i1, i64)
      emitrust.assign %c_nn = %unp#0 : !emitrust.lvalue<i1>
      emitrust.assign %c_idx = %unp#1 : !emitrust.lvalue<i64>
      scf.yield
    }

    %result = emitrust.load %sum : (!emitrust.lvalue<i32>) -> i32
    emitrust.call_opaque "print!"(%result) {args = ["{}\0A", 0 : index]} : (i32) -> ()
    %zero = emitrust.constant <0 : i32> : i32
    return %zero : i32
  }
  emitrust.verbatim "fn __emitrust_pool_opt(some: bool, idx: i64) -> Option<usize> {\0A    if some { Some(idx as usize) } else { None }\0A}"
  emitrust.verbatim "fn __emitrust_pool_unpack(o: Option<usize>) -> (bool, i64) {\0A    match o { Some(x) => (true, x as i64), None => (false, 0) }\0A}"
}

// No high-level container ops survive the pass.
// CHECK-NOT: emitrust.collection
// CHECK-NOT: emitrust.collection_push
// CHECK-NOT: emitrust.collection_at
