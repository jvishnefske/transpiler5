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
///
/// All yield lowerings preserve the parallel-assignment semantics of MLIR
/// block-argument rebinding: when a yielded source aliases a carried let
/// that an earlier assignment in the sequence would clobber (the lost-copy
/// problem), the sources are staged into fresh immutable temporaries before
/// any carried let is written.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Conversion/SCFToEmitRust.h"

#include "EmitRust/Conversion/EmitRustTypeConverter.h"
#include "EmitRust/EmitRustOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
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
    // W2.23: a STRUCT result -- the early-return structurization of a
    // struct-returning function (`if (pick) return p; return q;`, the
    // two-return copy shape) yields the struct through the scf.if -- takes
    // `Name::default()`, the exact FR-113 enum precedent one paragraph up
    // in getDefaultValueAttr: every emitted struct_def carries a Default
    // (derived or explicit), and the placeholder is dead -- the lowering
    // overwrites it on every branch, and for a droppy struct the
    // FR-61/FR-105 deferred-binding machinery elides the dead initializer
    // rather than dropping it (byte-diffed: no phantom destructor run;
    // test/EndToEnd/cpp-copy-dtor.cpp). Kept local to the SCF lowering so
    // `ub.poison` of struct type stays unsupported.
    if (!defaultValue)
      if (auto structType = dyn_cast<emitrust::StructType>(resultType))
        defaultValue = emitrust::OpaqueAttr::get(
            structType.getContext(),
            (structType.getName() + "::default()").str());
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

/// Returns true when emitting the assignments `lets[i] = values[i]` in
/// sequence could read an already-clobbered source, i.e. when some source
/// is a destination let that an earlier assignment in the sequence writes
/// (there exist indices i < j with lets[i] == values[j]). Every source
/// that is not itself one of the destination lets is an SSA value already
/// materialized into its own binding before the assignment sequence, so
/// only destination lets appearing as later sources can observe an
/// earlier write.
static bool sequentialAssignmentClobbersSource(ValueRange values,
                                               ValueRange lets) {
  for (auto [j, value] : llvm::enumerate(values))
    for (auto [i, let] : llvm::enumerate(lets)) {
      if (i >= j)
        break;
      if (let == value)
        return true;
    }
  return false;
}

/// Creates the assignments `lets[i] = values[i]` at the current insertion
/// point of `rewriter` with parallel-assignment semantics: every source is
/// read before any destination is written. MLIR region yields and loop
/// back-edges rebind all block arguments simultaneously, so naive
/// sequential assignments exhibit the classic lost-copy problem of SSA
/// destruction: a loop-carried rotation such as `a, b = b, a + b` emitted
/// sequentially reads the freshly clobbered `a`. When no source aliases an
/// earlier destination (see `sequentialAssignmentClobbersSource`) the
/// assignments are emitted directly; otherwise every source is first
/// captured into a fresh immutable temporary let and the destinations are
/// then assigned from those temporaries.
static void assignValues(ValueRange values, ValueRange lets,
                         ConversionPatternRewriter &rewriter, Location loc) {
  if (!sequentialAssignmentClobbersSource(values, lets)) {
    for (auto [value, let] : llvm::zip(values, lets))
      rewriter.create<emitrust::AssignOp>(loc, let, value);
    return;
  }
  SmallVector<Value> temporaries;
  for (Value value : values)
    temporaries.push_back(rewriter.create<emitrust::LetOp>(
        loc, value.getType(), value, /*is_mut=*/false));
  for (auto [temporary, let] : llvm::zip(temporaries, lets))
    rewriter.create<emitrust::AssignOp>(loc, let, temporary);
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

    // scf.for is half-open by construction, so `inclusive` is never set on
    // this path.
    auto loweredFor = rewriter.create<emitrust::ForOp>(
        loc, adaptor.getLowerBound(), adaptor.getUpperBound(),
        adaptor.getStep(), /*inclusive=*/false);

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

/// FR-61c: whether `op` may live in an `emitrust.while` condition region --
/// the conversion-time analogue of the emitter's FR-61d pure set. Both the
/// pre-conversion arith spellings and the already-converted emitrust
/// spellings can appear during partial conversion. No loads and no calls:
/// the condition region's chain must be pure so re-evaluating it at each
/// `while` head test is exactly the before-region's semantics.
static bool isLiftableConditionOp(Operation *op) {
  if (isa<arith::CmpIOp, arith::CmpFOp, arith::AddIOp, arith::SubIOp,
          arith::MulIOp, arith::DivSIOp, arith::DivUIOp, arith::RemSIOp,
          arith::RemUIOp, arith::AndIOp, arith::OrIOp, arith::XOrIOp,
          arith::ShLIOp, arith::ShRSIOp, arith::ShRUIOp, arith::ConstantOp,
          arith::ExtSIOp, arith::ExtUIOp, arith::TruncIOp,
          arith::IndexCastOp, arith::AddFOp, arith::SubFOp, arith::MulFOp,
          arith::DivFOp, arith::SIToFPOp, arith::FPToSIOp>(op))
    return true;
  return isa<emitrust::ConstantOp, emitrust::AddOp, emitrust::SubOp,
             emitrust::MulOp, emitrust::DivOp, emitrust::RemOp,
             emitrust::AndOp, emitrust::OrOp, emitrust::XorOp,
             emitrust::ShlOp, emitrust::ShrOp, emitrust::CmpOp,
             emitrust::CastOp, emitrust::BitcastOp>(op);
}

/// Converts `scf.while`. A loop whose before-region is a pure single-use
/// op chain ending in `scf.condition`, and whose forwarded condition
/// arguments are all loop-invariant or carried values, lifts to
/// `emitrust.while` (FR-61c): the before-region becomes the condition
/// region (folded into the Rust `while` head by the emitter), the
/// after-region becomes the body, and the loop results are the carried
/// lets themselves -- no body-if, no exit copies, no tail break.
/// Everything else keeps the `emitrust.loop` lowering below: the carried
/// values become mutable lets initialized from the while operands; the
/// results become default-initialized mutable lets. Inside the loop, the
/// before-region runs first; when the condition is false an `emitrust.if`
/// assigns the result lets from the condition arguments and breaks;
/// otherwise the after-region runs and assigns the carried lets at its
/// `scf.yield`.
struct WhileLowering : public OpConversionPattern<scf::WhileOp> {
  using OpConversionPattern<scf::WhileOp>::OpConversionPattern;

  /// FR-61c: the recognized liftable shape. `lift-cf-to-scf` emits a C
  /// `while` as a before-region holding the pure condition prefix, ONE
  /// `scf.if` guarded by the condition (the loop body in its then-arm,
  /// exit defaults in its else-arm yield), and `scf.condition` forwarding
  /// if-results / carried args / invariants; the after-region forwards.
  /// The degenerate form without the body-if (everything in the
  /// after-region) is the same shape with `bodyIf == nullptr`.
  struct WhileShape {
    scf::IfOp bodyIf; // null in the degenerate form
    Value cond;       // the scf-level condition value
  };

  /// FR-61c qualification. Holds when:
  ///  - every before-region op outside the body-if is pure with a single
  ///    result used only inside the before block (constants are exempt
  ///    from the use count -- the emitter duplicates them);
  ///  - the last op before `scf.condition` is either nothing or ONE
  ///    `scf.if` guarded by the same condition `scf.condition` tests,
  ///    whose results are used only as condition args and whose else-arm
  ///    is a bare `scf.yield` of before-args or loop-invariant values
  ///    (those become the loop's exit values, nameable after the while);
  ///  - every `scf.condition` arg is an if-result, a before-block
  ///    argument, or defined outside the loop.
  std::optional<WhileShape> qualifiesForWhileLift(scf::WhileOp whileOp) const {
    Block *beforeBlock = whileOp.getBeforeBody();
    auto conditionOp = cast<scf::ConditionOp>(beforeBlock->getTerminator());
    Value cond = conditionOp.getCondition();

    auto bodyIf = dyn_cast_or_null<scf::IfOp>(conditionOp->getPrevNode());
    if (bodyIf) {
      if (bodyIf.getCondition() != cond)
        return std::nullopt; // a differently-guarded if is a real statement
      if (!bodyIf.getThenRegion().hasOneBlock())
        return std::nullopt;
      // The else-arm must be a bare yield (its operands become the exit
      // values); its operands must stay nameable after the loop.
      if (bodyIf.getNumResults() != 0) {
        if (!bodyIf.getElseRegion().hasOneBlock())
          return std::nullopt;
        Block &elseBlock = bodyIf.getElseRegion().front();
        if (!llvm::hasSingleElement(elseBlock))
          return std::nullopt; // real else ops would have to run at exit
        for (Value v : cast<scf::YieldOp>(elseBlock.getTerminator())
                           .getOperands()) {
          if (auto blockArg = dyn_cast<BlockArgument>(v)) {
            if (blockArg.getOwner() != beforeBlock)
              return std::nullopt;
            continue;
          }
          if (Operation *def = v.getDefiningOp())
            if (whileOp->isAncestor(def))
              return std::nullopt;
        }
      }
      // If-results may only feed the condition terminator.
      for (Value result : bodyIf.getResults())
        for (Operation *user : result.getUsers())
          if (user != conditionOp)
            return std::nullopt;
      // The condition may only guard the body-if and the terminator.
      for (Operation *user : cond.getUsers())
        if (user != bodyIf && user != conditionOp)
          return std::nullopt;
    }

    // The pure condition prefix: everything before the body-if (or the
    // terminator in the degenerate form).
    Operation *prefixEnd =
        bodyIf ? bodyIf.getOperation() : conditionOp.getOperation();
    for (Operation &op :
         llvm::make_range(beforeBlock->begin(), prefixEnd->getIterator())) {
      if (!isLiftableConditionOp(&op) || op.getNumResults() != 1)
        return std::nullopt;
      if (isa<arith::ConstantOp, emitrust::ConstantOp>(op))
        continue;
      Value result = op.getResult(0);
      if (result == cond) {
        // The condition itself: consumed by the (optional) body-if and
        // the terminator, nothing else (checked above when bodyIf
        // exists; enforce here for the degenerate form).
        for (Operation *user : result.getUsers())
          if (user != conditionOp && user != bodyIf)
            return std::nullopt;
        continue;
      }
      if (!result.hasOneUse())
        return std::nullopt;
      Operation *user = *result.getUsers().begin();
      if (user->getBlock() != beforeBlock)
        return std::nullopt; // leaks into the body: not a condition op
    }

    for (Value arg : conditionOp.getArgs()) {
      if (bodyIf && arg.getDefiningOp() == bodyIf.getOperation())
        continue;
      if (auto blockArg = dyn_cast<BlockArgument>(arg)) {
        if (blockArg.getOwner() != beforeBlock)
          return std::nullopt;
        continue;
      }
      if (Operation *def = arg.getDefiningOp())
        if (whileOp->isAncestor(def))
          return std::nullopt; // computed in-loop: unnameable after it
    }
    return WhileShape{bodyIf, cond};
  }

  /// FR-61c: builds the `emitrust.while` for a qualified loop. The
  /// condition prefix becomes the condition region; the body-if's
  /// then-arm (when present) plus the after-region ops become the body,
  /// ending in the backedge assignments; the else-arm yields (or the
  /// carried lets themselves) become the loop's replacement values.
  LogicalResult liftToWhile(scf::WhileOp whileOp, WhileShape shape,
                            ConversionPatternRewriter &rewriter,
                            SmallVector<Value> &carriedLets) const {
    Location loc = whileOp.getLoc();
    auto lifted = rewriter.create<emitrust::WhileOp>(loc);

    // Condition region: the before-block, its arguments mapped to the
    // carried lets, its scf.condition replaced by emitrust.condition.
    Region &conditionRegion = lifted.getCondition();
    Block *conditionEntry;
    {
      OpBuilder::InsertionGuard guard(rewriter);
      conditionEntry = rewriter.createBlock(&conditionRegion);
    }
    Block *beforeBlock = whileOp.getBeforeBody();
    rewriter.inlineRegionBefore(whileOp.getBefore(), conditionRegion,
                                conditionRegion.end());
    rewriter.mergeBlocks(beforeBlock, conditionEntry, carriedLets);

    auto conditionOp = cast<scf::ConditionOp>(conditionEntry->getTerminator());
    scf::IfOp bodyIf = shape.bodyIf;

    // Collect the then-arm and else-arm yield operands (remapped) before
    // anything moves, plus the terminator condition.
    SmallVector<Value> thenYields, elseYields;
    Operation *thenTerminator = nullptr;
    if (bodyIf) {
      thenTerminator = bodyIf.getThenRegion().front().getTerminator();
      if (failed(rewriter.getRemappedValues(
              cast<scf::YieldOp>(thenTerminator).getOperands(), thenYields)))
        return rewriter.notifyMatchFailure(whileOp,
                                           "failed to remap then yields");
      if (bodyIf.getNumResults() != 0 &&
          failed(rewriter.getRemappedValues(
              cast<scf::YieldOp>(
                  bodyIf.getElseRegion().front().getTerminator())
                  .getOperands(),
              elseYields)))
        return rewriter.notifyMatchFailure(whileOp,
                                           "failed to remap else yields");
    }
    Value condition = rewriter.getRemappedValue(conditionOp.getCondition());
    if (!condition)
      return rewriter.notifyMatchFailure(whileOp,
                                         "failed to remap the condition");

    // Maps one scf.condition argument to its value on the CONTINUE edge
    // (body-if results are the then-arm yields) or the EXIT edge (they are
    // the else-arm yields).
    auto mapConditionArg = [&](Value arg, bool exiting) -> Value {
      if (bodyIf && arg.getDefiningOp() == bodyIf.getOperation()) {
        unsigned index = cast<OpResult>(arg).getResultNumber();
        return exiting ? elseYields[index] : thenYields[index];
      }
      return rewriter.getRemappedValue(arg);
    };
    SmallVector<Value> continueArgs, exitArgs;
    for (Value arg : conditionOp.getArgs()) {
      continueArgs.push_back(mapConditionArg(arg, /*exiting=*/false));
      exitArgs.push_back(mapConditionArg(arg, /*exiting=*/true));
      if (!continueArgs.back() || !exitArgs.back())
        return rewriter.notifyMatchFailure(whileOp,
                                           "failed to remap condition args");
    }

    // Body region: the then-arm ops (when present), then the after-region
    // ops with their arguments mapped to the continue-edge values, then
    // the backedge assignments.
    Region &bodyRegion = lifted.getBody();
    Block *bodyEntry;
    {
      OpBuilder::InsertionGuard guard(rewriter);
      bodyEntry = rewriter.createBlock(&bodyRegion);
    }
    if (bodyIf) {
      Block *thenBlock = &bodyIf.getThenRegion().front();
      rewriter.inlineRegionBefore(bodyIf.getThenRegion(), bodyRegion,
                                  bodyRegion.end());
      rewriter.mergeBlocks(thenBlock, bodyEntry, /*argValues=*/{});
      rewriter.eraseOp(thenTerminator);
    }
    Block *afterBlock = whileOp.getAfterBody();
    Operation *afterTerminator = afterBlock->getTerminator();
    rewriter.inlineRegionBefore(whileOp.getAfter(), bodyRegion,
                                bodyRegion.end());
    // Splice the after ops behind the then ops in the entry block.
    Block *afterEntry = &*std::next(bodyRegion.begin());
    rewriter.mergeBlocks(afterEntry, bodyEntry, continueArgs);
    if (failed(lowerYield(whileOp, carriedLets, rewriter,
                          cast<scf::YieldOp>(afterTerminator))))
      return failure();

    // Retire the condition terminator and the (now then-less) body-if.
    {
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPoint(conditionOp);
      rewriter.create<emitrust::ConditionOp>(loc, condition);
    }
    rewriter.eraseOp(conditionOp);
    if (bodyIf)
      rewriter.eraseOp(bodyIf);

    // The loop results are the exit-edge values: else-arm yields, carried
    // lets, or loop invariants -- all nameable after the while.
    rewriter.replaceOp(whileOp, exitArgs);
    return success();
  }

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

    // FR-61c: a pure-condition loop lifts to `emitrust.while` instead.
    if (std::optional<WhileShape> shape = qualifiesForWhileLift(whileOp)) {
      if (::getenv("EMITRUST_WHILE_LIFT_STATS"))
        llvm::errs() << "[while-lift] lifted\n";
      return liftToWhile(whileOp, *shape, rewriter, carriedLets);
    }
    if (::getenv("EMITRUST_WHILE_LIFT_STATS"))
      llvm::errs() << "[while-lift] fallback\n";

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
