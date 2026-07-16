//===- SCFToEmitRust.cpp - SCF to EmitRust conversion ---------------------===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements the conversion from the SCF dialect to the EmitRust dialect,
/// preserving structured control flow. SSA results of the SCF operations
/// are modeled as mutable `emitrust.let` bindings that the inlined regions
/// assign into:
///
/// - `scf.if` with N results becomes N default-initialized mutable lets
///   followed by a result-less `emitrust.if`; each `scf.yield` becomes
///   assignments into the lets plus an `emitrust.yield` terminator.
/// - `scf.while` becomes mutable lets for the carried values (initialized
///   from the while operands) and for the results (default-initialized),
///   and an `emitrust.loop`: the before-region runs first, the negated
///   condition guards an `emitrust.if` that assigns the result lets and
///   breaks, and the after-region follows, assigning the carried lets at
///   its `scf.yield`.
/// - `scf.for` becomes mutable lets for the iteration arguments and an
///   `emitrust.for` whose body assigns the lets at `scf.yield`.
/// - `scf.index_switch` becomes default-initialized mutable lets and an
///   `emitrust.switch` over the same discriminator; the case and default
///   regions are inlined into the corresponding switch regions and assign
///   the lets at their `scf.yield`.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Conversion/SCFToEmitRust.h"

#include "EmitRust/Conversion/EmitRustTypeConverter.h"
#include "EmitRust/EmitRustOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/STLExtras.h"

namespace mlir {
namespace emitrust {
#define GEN_PASS_DEF_CONVERTSCFTOEMITRUST
#include "EmitRust/Conversion/Passes.h.inc"
} // namespace emitrust
} // namespace mlir

using namespace mlir;
using namespace mlir::emitrust;

//===----------------------------------------------------------------------===//
// Shared helpers
//===----------------------------------------------------------------------===//

/// Creates one mutable `emitrust.let` binding per result of `op`, each
/// initialized with an `emitrust.constant` holding the default value of the
/// converted result type. The bindings are created right before `op` and
/// later replace its results.
template <typename OpTy>
static LogicalResult
createDefaultInitializedLets(OpTy op, const TypeConverter *typeConverter,
                             ConversionPatternRewriter &rewriter,
                             SmallVector<Value> &resultLets) {
  if (op.getNumResults() == 0)
    return success();

  Location loc = op.getLoc();
  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPoint(op);

  for (OpResult result : op.getResults()) {
    Type resultType = typeConverter->convertType(result.getType());
    if (!resultType)
      return rewriter.notifyMatchFailure(op, "result type conversion failed");
    Attribute defaultValue = getDefaultValueAttr(resultType);
    if (!defaultValue)
      return rewriter.notifyMatchFailure(
          op, "no default value for the result type");
    Value init =
        rewriter.create<emitrust::ConstantOp>(loc, resultType, defaultValue);
    resultLets.push_back(rewriter.create<emitrust::LetOp>(loc, resultType,
                                                          init,
                                                          /*is_mut=*/true));
  }
  return success();
}

/// Creates one `emitrust.assign` per value/let pair at the current
/// insertion point of `rewriter`.
static void assignValues(ValueRange values, ValueRange lets,
                         ConversionPatternRewriter &rewriter, Location loc) {
  for (auto [value, let] : llvm::zip(values, lets))
    rewriter.create<emitrust::AssignOp>(loc, let, value);
}

/// Replaces the `scf.yield` terminator `yield` with assignments of its
/// (remapped) operands into `resultLets`, followed by an `emitrust.yield`.
static LogicalResult lowerYield(Operation *op, ValueRange resultLets,
                                ConversionPatternRewriter &rewriter,
                                scf::YieldOp yield) {
  Location loc = yield.getLoc();

  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPoint(yield);

  SmallVector<Value> yieldOperands;
  if (failed(rewriter.getRemappedValues(yield.getOperands(), yieldOperands)))
    return rewriter.notifyMatchFailure(op, "failed to remap yield operands");

  assignValues(yieldOperands, resultLets, rewriter, loc);

  rewriter.create<emitrust::YieldOp>(loc);
  rewriter.eraseOp(yield);
  return success();
}

//===----------------------------------------------------------------------===//
// Conversion patterns
//===----------------------------------------------------------------------===//

namespace {

/// Converts `scf.if` into `emitrust.if`, modeling result values as mutable
/// `emitrust.let` bindings assigned within the then and else regions. A
/// result-less `scf.if` converts to a plain `emitrust.if`.
struct IfLowering : public OpConversionPattern<scf::IfOp> {
  using OpConversionPattern<scf::IfOp>::OpConversionPattern;

  /// Rewrites the if, inlining both regions.
  LogicalResult
  matchAndRewrite(scf::IfOp ifOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // Mutable lets that stand in for the scf.if results.
    SmallVector<Value> resultLets;
    if (failed(createDefaultInitializedLets(ifOp, getTypeConverter(), rewriter,
                                            resultLets)))
      return failure();

    // Inlines an scf.if region into the corresponding emitrust.if region
    // and rewrites its scf.yield into assignments plus emitrust.yield.
    auto lowerRegion = [&resultLets, &rewriter,
                        &ifOp](Region &region,
                               Region &loweredRegion) -> LogicalResult {
      rewriter.inlineRegionBefore(region, loweredRegion, loweredRegion.end());
      Operation *terminator = loweredRegion.back().getTerminator();
      return lowerYield(ifOp, resultLets, rewriter,
                        cast<scf::YieldOp>(terminator));
    };

    Region &thenRegion = adaptor.getThenRegion();
    Region &elseRegion = adaptor.getElseRegion();
    bool hasElseBlock = !elseRegion.empty();

    auto loweredIf =
        rewriter.create<emitrust::IfOp>(ifOp.getLoc(), adaptor.getCondition());

    if (failed(lowerRegion(thenRegion, loweredIf.getThenRegion())))
      return failure();
    if (hasElseBlock &&
        failed(lowerRegion(elseRegion, loweredIf.getElseRegion())))
      return failure();

    rewriter.replaceOp(ifOp, resultLets);
    return success();
  }
};

/// Converts `scf.for` into `emitrust.for`, modeling the iteration arguments
/// (and thus the loop results) as mutable `emitrust.let` bindings that the
/// body assigns at `scf.yield`.
struct ForLowering : public OpConversionPattern<scf::ForOp> {
  using OpConversionPattern<scf::ForOp>::OpConversionPattern;

  /// Rewrites the for loop, merging its body into the new loop body.
  LogicalResult
  matchAndRewrite(scf::ForOp forOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = forOp.getLoc();

    // Mutable lets carrying the iteration arguments across iterations;
    // they also replace the loop results afterwards.
    SmallVector<Value> iterLets;
    for (auto [init, result] :
         llvm::zip(adaptor.getInitArgs(), forOp.getResults())) {
      Type resultType = getTypeConverter()->convertType(result.getType());
      if (!resultType)
        return rewriter.notifyMatchFailure(forOp,
                                           "result type conversion failed");
      iterLets.push_back(rewriter.create<emitrust::LetOp>(loc, resultType,
                                                          init,
                                                          /*is_mut=*/true));
    }

    auto loweredFor = rewriter.create<emitrust::ForOp>(
        loc, adaptor.getLowerBound(), adaptor.getUpperBound(),
        adaptor.getStep());

    // The default builder leaves the region empty: create the body block
    // with the induction-variable argument ourselves.
    Region &loweredRegion = loweredFor.getRegion();
    Block *loweredBody;
    {
      OpBuilder::InsertionGuard guard(rewriter);
      loweredBody = rewriter.createBlock(
          &loweredRegion, loweredRegion.end(),
          TypeRange{adaptor.getLowerBound().getType()}, {loc});
    }

    // Merge the scf.for body into the new body, mapping the induction
    // variable to the new block argument and the iteration arguments to
    // the mutable lets.
    SmallVector<Value> replacements;
    replacements.push_back(loweredBody->getArgument(0));
    llvm::append_range(replacements, iterLets);
    rewriter.mergeBlocks(forOp.getBody(), loweredBody, replacements);

    if (failed(lowerYield(forOp, iterLets, rewriter,
                          cast<scf::YieldOp>(loweredBody->getTerminator()))))
      return failure();

    rewriter.replaceOp(forOp, iterLets);
    return success();
  }
};

/// Converts `scf.while` into `emitrust.loop`. The carried values become
/// mutable lets initialized from the while operands; the results become
/// default-initialized mutable lets. Inside the loop, the before-region
/// runs first; when the condition is false an `emitrust.if` assigns the
/// result lets from the condition arguments and breaks; otherwise the
/// after-region runs and assigns the carried lets at its `scf.yield`.
struct WhileLowering : public OpConversionPattern<scf::WhileOp> {
  using OpConversionPattern<scf::WhileOp>::OpConversionPattern;

  /// Rewrites the while loop into an infinite loop with a break.
  LogicalResult
  matchAndRewrite(scf::WhileOp whileOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = whileOp.getLoc();

    if (!whileOp.getBefore().hasOneBlock() ||
        !whileOp.getAfter().hasOneBlock())
      return rewriter.notifyMatchFailure(
          whileOp, "expected single-block before and after regions");

    // Mutable lets for the carried values, initialized from the operands.
    SmallVector<Value> carriedLets;
    for (auto [init, argType] :
         llvm::zip(adaptor.getInits(), whileOp.getBefore().getArgumentTypes())) {
      Type carriedType = getTypeConverter()->convertType(argType);
      if (!carriedType)
        return rewriter.notifyMatchFailure(whileOp,
                                           "carried type conversion failed");
      carriedLets.push_back(rewriter.create<emitrust::LetOp>(loc, carriedType,
                                                             init,
                                                             /*is_mut=*/true));
    }

    // Default-initialized mutable lets that stand in for the results.
    SmallVector<Value> resultLets;
    if (failed(createDefaultInitializedLets(whileOp, getTypeConverter(),
                                            rewriter, resultLets)))
      return failure();

    // The loop body: before-region ops, then the negated-condition exit,
    // then the after-region ops, then assignments of the carried lets.
    auto loop = rewriter.create<emitrust::LoopOp>(loc);
    Region &loopRegion = loop.getRegion();
    Block *entryBlock;
    {
      OpBuilder::InsertionGuard guard(rewriter);
      entryBlock = rewriter.createBlock(&loopRegion);
    }

    // Merge the before-block into the loop entry block, mapping its
    // arguments to the carried lets.
    Block *beforeBlock = whileOp.getBeforeBody();
    rewriter.inlineRegionBefore(whileOp.getBefore(), loopRegion,
                                loopRegion.end());
    rewriter.mergeBlocks(beforeBlock, entryBlock, carriedLets);

    auto conditionOp = cast<scf::ConditionOp>(entryBlock->getTerminator());
    SmallVector<Value> conditionArgs;
    if (failed(rewriter.getRemappedValues(conditionOp.getArgs(),
                                          conditionArgs)))
      return rewriter.notifyMatchFailure(whileOp,
                                         "failed to remap condition args");
    Value condition = rewriter.getRemappedValue(conditionOp.getCondition());
    if (!condition)
      return rewriter.notifyMatchFailure(whileOp,
                                         "failed to remap the condition");

    // Negate the condition (cmp eq against false) and break out of the
    // loop when it holds, assigning the result lets first.
    {
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPoint(conditionOp);
      Type i1Type = rewriter.getI1Type();
      Value falseValue = rewriter.create<emitrust::ConstantOp>(
          loc, i1Type, rewriter.getBoolAttr(false));
      Value negated = rewriter.create<emitrust::CmpOp>(
          loc, i1Type, emitrust::CmpPredicate::eq, condition, falseValue);
      auto exitIf = rewriter.create<emitrust::IfOp>(loc, negated);
      rewriter.createBlock(&exitIf.getThenRegion());
      assignValues(conditionArgs, resultLets, rewriter, loc);
      rewriter.create<emitrust::BreakOp>(loc);
      rewriter.create<emitrust::YieldOp>(loc);
    }

    // Inline the after-block right before the (soon to be erased)
    // condition terminator, mapping its arguments to the condition args.
    Block *afterBlock = whileOp.getAfterBody();
    Operation *afterTerminator = afterBlock->getTerminator();
    rewriter.inlineBlockBefore(afterBlock, conditionOp, conditionArgs);

    // Rewrite the after-region scf.yield into assignments of the carried
    // lets plus the emitrust.yield loop terminator.
    if (failed(lowerYield(whileOp, carriedLets, rewriter,
                          cast<scf::YieldOp>(afterTerminator))))
      return failure();
    rewriter.eraseOp(conditionOp);

    rewriter.replaceOp(whileOp, resultLets);
    return success();
  }
};

/// Converts `scf.index_switch` into `emitrust.switch` over the same
/// discriminator and case values. Every case region and the default region
/// is inlined into the corresponding switch region. Result values are
/// modeled as default-initialized mutable `emitrust.let` bindings assigned
/// at each region's `scf.yield`, mirroring the `scf.if` lowering.
struct IndexSwitchLowering : public OpConversionPattern<scf::IndexSwitchOp> {
  using OpConversionPattern<scf::IndexSwitchOp>::OpConversionPattern;

  /// Rewrites the switch, inlining every case region and the default region.
  LogicalResult
  matchAndRewrite(scf::IndexSwitchOp switchOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = switchOp.getLoc();

    // Mutable lets that stand in for the scf.index_switch results.
    SmallVector<Value> resultLets;
    if (failed(createDefaultInitializedLets(switchOp, getTypeConverter(),
                                            rewriter, resultLets)))
      return failure();

    // The discriminator keeps its converted type (index renders as usize);
    // the case values carry over verbatim.
    auto loweredSwitch = rewriter.create<emitrust::SwitchOp>(
        loc, adaptor.getArg(), switchOp.getCasesAttr(),
        switchOp.getCases().size());

    // Inlines one scf region into an emitrust.switch region and rewrites
    // its scf.yield into assignments plus emitrust.yield.
    auto lowerRegion = [&resultLets, &rewriter,
                        &switchOp](Region &region,
                                   Region &loweredRegion) -> LogicalResult {
      rewriter.inlineRegionBefore(region, loweredRegion, loweredRegion.end());
      Operation *terminator = loweredRegion.back().getTerminator();
      return lowerYield(switchOp, resultLets, rewriter,
                        cast<scf::YieldOp>(terminator));
    };

    for (auto [caseRegion, loweredRegion] :
         llvm::zip(adaptor.getCaseRegions(), loweredSwitch.getCaseRegions())) {
      if (failed(lowerRegion(*caseRegion, loweredRegion)))
        return failure();
    }
    if (failed(lowerRegion(adaptor.getDefaultRegion(),
                           loweredSwitch.getDefaultRegion())))
      return failure();

    rewriter.replaceOp(switchOp, resultLets);
    return success();
  }
};

} // namespace

//===----------------------------------------------------------------------===//
// Pattern population
//===----------------------------------------------------------------------===//

/// Collects the patterns converting SCF operations into EmitRust
/// structured control flow.
void mlir::emitrust::populateSCFToEmitRustPatterns(
    TypeConverter &typeConverter, RewritePatternSet &patterns) {
  patterns.add<IfLowering, ForLowering, WhileLowering, IndexSwitchLowering>(
      typeConverter, patterns.getContext());
}

//===----------------------------------------------------------------------===//
// Pass definition
//===----------------------------------------------------------------------===//

namespace {

/// The convert-scf-to-emitrust pass: applies the SCF-to-EmitRust patterns
/// under a partial conversion in which scf.if, scf.for, scf.while, and
/// scf.index_switch are illegal. Their scf.yield/scf.condition terminators
/// are erased as part of the parent rewrites.
struct ConvertSCFToEmitRust
    : public emitrust::impl::ConvertSCFToEmitRustBase<ConvertSCFToEmitRust> {
  /// Runs the partial conversion and fails the pass on leftover SCF ops.
  void runOnOperation() override {
    ConversionTarget target(getContext());
    target.addLegalDialect<EmitRustDialect>();
    target.addIllegalOp<scf::IfOp, scf::ForOp, scf::WhileOp,
                        scf::IndexSwitchOp>();

    TypeConverter typeConverter;
    populateEmitRustTypeConverter(typeConverter);

    RewritePatternSet patterns(&getContext());
    populateSCFToEmitRustPatterns(typeConverter, patterns);

    if (failed(applyPartialConversion(getOperation(), target,
                                      std::move(patterns))))
      signalPassFailure();
  }
};

} // namespace
