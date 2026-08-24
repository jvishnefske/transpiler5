// FR-130: the pinned import-to-emitrust lowering pipeline is ONE definition
// (`emitrust::buildLoweringPipeline`) that both emitrust-cc and emitrust-clang
// call. It used to be hand-copied into each driver, and the two copies had
// diverged with NO test able to see the difference -- this file is that test.
// It pins two things the drivers otherwise pin only indirectly:
//
//  1. every MANDATORY stage runs, and in the pinned order: a `memref.alloca`
//     accumulator is promoted (mem2reg), unstructured `cf` branches become an
//     scf loop (lift-cf-to-scf), the canonicalizer folds, and the whole thing
//     converts to the emitrust dialect (convert-to-emitrust). A dropped or
//     reordered stage leaves core-dialect ops behind and the CHECK-NOT fires.
//
//  2. both OPTIONAL stages default to OFF. The FR-52 lowering is the one that
//     matters: emitrust-clang imports with `deferExternals` (design.md FR-57a
//     -- "defer takes precedence over the FR-52 trait policy"), so its
//     unresolved externals must survive as `emitrust.extern_decl` link
//     obligations for the FR-58 link step. If a future edit made the FR-52
//     lowering unconditional, the marker below would be consumed into an
//     Externals trait and the shard would resolve its externals too early.
//
// RUN: emitrust-opt %s --emitrust-lowering | FileCheck %s

// CHECK-LABEL: emitrust.func @sum_to
// The alloca-backed accumulator and induction variable are promoted to SSA
// (mem2reg) and the cf.br backedge is lifted to a structured loop, so nothing
// from memref/cf/arith survives the pipeline.
// CHECK-NOT:   memref.
// CHECK-NOT:   cf.
// CHECK-NOT:   arith.
// CHECK:       emitrust.while
// CHECK:         emitrust.cmp  lt
// CHECK:         emitrust.add
// CHECK:       emitrust.return
func.func @sum_to(%n: i32) -> i32 {
  %c0 = arith.constant 0 : i32
  %c1 = arith.constant 1 : i32
  %acc = memref.alloca() : memref<i32>
  %i = memref.alloca() : memref<i32>
  memref.store %c0, %acc[] : memref<i32>
  memref.store %c0, %i[] : memref<i32>
  cf.br ^head

^head:
  %iv = memref.load %i[] : memref<i32>
  %cond = arith.cmpi slt, %iv, %n : i32
  cf.cond_br %cond, ^body, ^exit

^body:
  %a = memref.load %acc[] : memref<i32>
  %a2 = arith.addi %a, %iv : i32
  memref.store %a2, %acc[] : memref<i32>
  %i2 = arith.addi %iv, %c1 : i32
  memref.store %i2, %i[] : memref<i32>
  cf.br ^head

^exit:
  %res = memref.load %acc[] : memref<i32>
  return %res : i32
}

// The FR-52 external-requirement marker survives the DEFAULT pipeline
// untouched: no `Externals` trait is synthesized and the marked declaration is
// still there, carrying its marker, for the link step to resolve.
// CHECK-NOT:   emitrust.trait_def
// CHECK:       emitrust.func private @host_scale
// CHECK-SAME:  emitrust.external_requirement
func.func private @host_scale(i32) -> i32
    attributes {emitrust.external_requirement}
