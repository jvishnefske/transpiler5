//===- LoweringPipeline.cpp - The pinned lowering pipeline ------*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// FR-130: the ONE definition of the import-to-emitrust lowering pipeline.
///
/// It used to be hand-copied into emitrust-cc and emitrust-clang, and the two
/// copies had drifted: emitrust-clang was missing both the optional
/// range-refinement check and the FR-52 external-requirement lowering, with no
/// test that could see the difference. The pipeline is now stated once and
/// both drivers call it, so the only way they can differ is through the
/// explicit `LoweringPipelineOptions` fields.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Conversion/LoweringPipeline.h"

#include "EmitRust/Conversion/ConvertToEmitRust.h"
#include "EmitRust/Conversion/LowerContainers.h"
#include "EmitRust/Conversion/LowerExternalRequirements.h"
#include "EmitRust/Conversion/CanonicalRoundTrip.h"
#include "EmitRust/Conversion/RangeRefinementCheck.h"

#include "EmitRust/EmitRustDialect.h"
#include "EmitRust/EmitRustOps.h"
#include "EmitRust/EmitRustTypes.h"

#include "mlir/Conversion/ControlFlowToSCF/ControlFlowToSCF.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/Passes.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

namespace mlir {
namespace emitrust {

namespace {

/// FR-152: does a value of `type` own something whose Rust `Drop` runs at the
/// end of each loop iteration?
///
/// This is the FENCE on the hoist below, and it is the whole reason the hoist
/// is safe. Moving a place out of the loop makes it ONE binding for the whole
/// loop instead of one per iteration; for a plain scalar/struct/enum/fn-ptr
/// that is invisible (reading such a local before this iteration wrote it is
/// C UB, and Rust's own E0381 is the safe failure direction), but for a type
/// with a destructor it silently DELETES every drop but the last. Measured on
/// a `struct R` whose `~R` prints: the native emitted three `dtor` lines and
/// the hoisted crate emitted one. A compile-clean miscompile, so the shape is
/// rejected rather than hoisted.
///
/// `emitrust.opaque` (the STL locals -- `std::string`, `std::vector<T>`) is
/// treated as droppy unconditionally: the dialect knows nothing about the Rust
/// type behind the string, and every such type this importer produces owns a
/// heap allocation.
bool typeMayDrop(Operation *from, Type type,
                 llvm::DenseSet<StringRef> &visiting) {
  if (isa<OpaqueType>(type))
    return true;
  if (auto array = dyn_cast<ArrayType>(type))
    return typeMayDrop(from, array.getElementType(), visiting);
  auto structType = dyn_cast<StructType>(type);
  if (!structType)
    return false;
  StructDefOp def = StructDefOp::lookupFrom(from, structType.getName());
  // An unresolvable struct name is not something to be optimistic about.
  if (!def)
    return true;
  if (def->hasAttr(kHasDropAttrName))
    return true;
  if (!visiting.insert(structType.getName()).second)
    return false;
  for (Attribute fieldType : def.getFieldTypes())
    if (auto typeAttr = dyn_cast<TypeAttr>(fieldType))
      if (typeMayDrop(from, typeAttr.getValue(), visiting))
        return true;
  return false;
}

/// FR-152: does `root`, an SSA value defined inside `boundary`'s regions,
/// reach code OUTSIDE `boundary`?
///
/// A value cannot leave a region except through that region's terminator, so
/// this is a walk of the uses that follows every terminator forward into the
/// values it feeds -- a nested loop's results and block arguments, a nested
/// `scf.if`'s results. Following them matters because an inner loop can carry
/// a place out to an outer one, and only the OUTERMOST carry is the one that
/// makes the `scf.while` result illegal.
///
/// A terminator this does not know is treated as NOT escaping. That is the
/// direction that preserves today's behaviour exactly: a missed hoist leaves
/// the shape as the loud rejection it already is, whereas an invented one
/// would move a `let` in already-working output.
bool valueEscapes(Value root, Operation *boundary) {
  SmallVector<Value> worklist;
  llvm::DenseSet<Value> seen;
  auto push = [&](Value v) {
    if (seen.insert(v).second)
      worklist.push_back(v);
  };
  push(root);
  while (!worklist.empty()) {
    Value value = worklist.pop_back_val();
    for (OpOperand &use : value.getUses()) {
      Operation *user = use.getOwner();
      if (!boundary->isProperAncestor(user))
        return true;
      Operation *parent = user->getParentOp();
      unsigned index = use.getOperandNumber();
      if (isa<scf::ConditionOp>(user)) {
        auto whileOp = cast<scf::WhileOp>(parent);
        if (whileOp == boundary)
          return true;
        // Operand 0 is the i1 condition; operand i>0 becomes result i-1 and
        // after-region argument i-1 of the nested loop.
        push(whileOp.getResult(index - 1));
        push(whileOp.getAfterArguments()[index - 1]);
      } else if (isa<scf::YieldOp>(user)) {
        if (parent == boundary)
          return true;
        if (auto whileOp = dyn_cast<scf::WhileOp>(parent)) {
          push(whileOp.getBeforeArguments()[index]);
        } else if (auto forOp = dyn_cast<scf::ForOp>(parent)) {
          push(forOp.getResult(index));
          push(forOp.getRegionIterArg(index));
        } else if (isa<scf::IfOp, scf::IndexSwitchOp, scf::ExecuteRegionOp>(
                       parent)) {
          push(parent->getResult(index));
        }
      }
    }
  }
  return false;
}

/// FR-152: hoist a loop-body place whose value escapes its loop.
///
/// `lift-cf-to-scf` turns an unbounded C loop into an `scf.while`. A local the
/// importer routes to an `emitrust.variable` place (unsigned scalar, enum,
/// struct, function pointer, address-taken scalar -- see `emitLocalVar`) that
/// is DECLARED in the loop body and READ after the loop therefore leaves the
/// region as a loop-carried value, and the `scf.while` acquires an
/// `!emitrust.lvalue<T>` result. `SCFToEmitRust`'s `WhileLowering` has to
/// default-initialize every carried value before the loop, and
/// `getDefaultValueAttr` has no default for an lvalue type -- so the whole
/// function died with `failed to legalize operation 'scf.while'`.
///
/// A place has no per-iteration meaning of its own: it is the DECLARATION, and
/// the store into it stays where it was. So moving the declaration out of the
/// loop is exactly the C `unsigned char c; for (;;) { c = ...; }` spelling of
/// the same program. Once it is out, the carried value is loop-invariant and
/// the canonicalizer that already follows this pass removes it through MLIR's
/// own `RemoveLoopInvariantArgsFromBeforeBlock` /
/// `RemoveLoopInvariantValueYielded`; the result arity shrinks and the rest of
/// the pipeline is unchanged. No new op is needed anywhere.
///
/// Deliberately NOT handled, and left as the located rejection they already
/// are:
///   * a place carrying an `init` attribute, which would drag a
///     per-iteration reset out of the loop with the declaration (the exact
///     FR-155 miscompile), and a `const` place, which cannot be assigned
///     after its declaration at all;
///   * a place whose type may drop -- see `typeMayDrop`; those get the
///     located diagnostic below instead of a legalization failure;
///   * a place nested one region DEEPER than the `scf.while` body (a bounded
///     loop with an inner `return` puts it inside an `scf.if` and seeds the
///     carried value with `ub.poison`). That value is not loop-invariant, so
///     canonicalize cannot drop it and hoisting provably does not help; it
///     stays the located `ub.poison` rejection it is today.
struct HoistEscapingLoopPlacesPass
    : public PassWrapper<HoistEscapingLoopPlacesPass,
                         OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(HoistEscapingLoopPlacesPass)

  StringRef getArgument() const final {
    return "emitrust-hoist-escaping-loop-places";
  }
  StringRef getDescription() const final {
    return "FR-152: move an emitrust.variable place declared in an scf.while "
           "region, and escaping it, in front of the loop";
  }

  void runOnOperation() override {
    // A hoist can expose the next one: an inner loop's place lands in the
    // OUTER loop's region, where the same rule applies. Iterate to fixpoint.
    for (bool changed = true; changed;) {
      changed = false;
      SmallVector<VariableOp> hoist;
      bool rejected = false;
      getOperation().walk([&](VariableOp var) {
        if (var.getInitAttr() || var.getIsConst())
          return;
        Operation *owner = var->getParentRegion()->getParentOp();
        if (!isa<scf::WhileOp>(owner))
          return;
        if (!valueEscapes(var.getResult(), owner))
          return;
        llvm::DenseSet<StringRef> visiting;
        Type valueType =
            cast<LValueType>(var.getResult().getType()).getValueType();
        if (typeMayDrop(var, valueType, visiting)) {
          // Rejection is a feature: this shape is unsupported, and saying so
          // at the declaration beats the `failed to legalize operation
          // 'scf.while'` the pipeline would otherwise report three stages
          // later.
          std::string named;
          if (std::optional<StringRef> cName = var.getCName())
            named = (" '" + *cName + "'").str();
          InFlightDiagnostic diag =
              var.emitError() << "unsupported: local" << named
                              << " has a destructor, is declared inside a "
                                 "loop body, and escapes the loop";
          diag.attachNote()
              << "moving the declaration out of the loop would delete every "
                 "per-iteration drop but the last";
          rejected = true;
          return;
        }
        hoist.push_back(var);
      });
      if (rejected)
        return signalPassFailure();
      for (VariableOp var : hoist) {
        var->moveBefore(var->getParentRegion()->getParentOp());
        changed = true;
      }
    }
  }
};

} // namespace

void buildLoweringPipeline(OpPassManager &pm,
                           const LoweringPipelineOptions &options) {
  // FR-134: BEFORE every lowering stage, so what it certifies is the FRONT
  // END's module -- at this point a round-trip failure is still attributable
  // to the importer. Mutates nothing: it either passes silently or fails the
  // compile with its located diagnostic.
  if (options.canonicalRoundTrip)
    pm.addPass(createEmitRustCanonicalRoundTrip());
  // Lower the high-level container ops (the FR-39 node pool) to the concrete
  // `[T;CAP]` array + cursor shape FIRST, before mem2reg promotes the cursor,
  // so all downstream lowering is identical to inlining the pool directly.
  pm.addPass(createEmitRustLowerContainers());
  pm.addPass(mlir::createMem2Reg());
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(mlir::createLiftControlFlowToSCFPass());
  // FR-152: strictly BETWEEN the lift and the canonicalizer. The lift is what
  // creates the `!emitrust.lvalue<T>`-typed loop-carried value, and the
  // canonicalizer is what removes it once this pass has made it invariant.
  pm.addNestedPass<func::FuncOp>(std::make_unique<HoistEscapingLoopPlacesPass>());
  pm.addPass(mlir::createCanonicalizerPass());
  // Differential, and it clones internally -- so the main pipeline continues
  // unchanged after it; a violation fails the compile with the pass's located
  // diagnostic.
  if (options.checkRangeRefinement)
    pm.addPass(createEmitRustRangeRefinementCheck());
  pm.addPass(createConvertToEmitRust());
  // FR-52: strictly after the conversion, because the trait is expressed in
  // the EMITTED names and in the opaque string callees the conversion
  // produces. A no-op unless the importer marked an unresolved external,
  // which is why every existing crate stays byte-identical.
  if (options.lowerExternalRequirements)
    pm.addPass(createEmitRustLowerExternalRequirements());
}

void registerEmitRustLoweringPipeline() {
  // FR-152: also expose the hoist on its own, so lit can pin the IR shape it
  // produces instead of only its effect through the whole pipeline.
  static PassRegistration<HoistEscapingLoopPlacesPass> hoistEscapingLoopPlaces;
  PassPipelineRegistration<>(
      "emitrust-lowering",
      "The pinned import-to-emitrust lowering pipeline the drivers run",
      [](OpPassManager &pm) { buildLoweringPipeline(pm); });
}

} // namespace emitrust
} // namespace mlir
