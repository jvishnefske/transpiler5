//===- LowerContainers.cpp - Container op lowering --------------*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
// Lowers the backend-agnostic container ops
// (`emitrust.collection`/`collection_push`/`collection_at`) to the production
// array backend: a fixed `[T;CAP]` pool + a rank-0 `memref` i64 free cursor
// (the FR-39 node-pool shape).
//
//   * collection : an `emitrust.variable` of `[T;CAP]` plus a `memref.alloca`
//                  i64 cursor cell, stored 0.
//   * push       : load the cursor, take that index, store `index + 1` back;
//                  the CAP slots are pre-defaulted by the array's default
//                  value, so "append" is just a cursor bump.
//   * at         : an `emitrust.subscript` of the pool at the index.
//
// This reproduces the exact legacy shape the importer previously inlined at
// recognition time; the pass runs first in the emitrust-cc pipeline, before
// mem2reg promotes the cursor, so all downstream lowering is byte-for-byte
// identical. The collection place, its cursor, and its element type are
// threaded from the `collection` op to its `push`/`at` users through a side
// table keyed by the (RAUW'd) place value; the greedy driver retries
// `push`/`at` until the `collection` has lowered. A growable `Vec<T>` backend
// is deferred to the heap RFC (design.md W4.5).
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Conversion/LowerContainers.h"
#include "EmitRust/EmitRustOps.h"
#include "EmitRust/EmitRustTypes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "llvm/ADT/DenseMap.h"

namespace mlir {
namespace emitrust {
#define GEN_PASS_DEF_EMITRUSTLOWERCONTAINERS
#include "EmitRust/Conversion/Passes.h.inc"
} // namespace emitrust
} // namespace mlir

using namespace mlir;
using namespace mlir::emitrust;

namespace {

/// The per-collection state threaded from `collection` to `push`/`at`.
struct CollectionState {
  Value place;  // the `[T;CAP]` pool lvalue
  Value cursor; // the rank-0 `memref<i64>` free cursor cell
};

using StateMap = llvm::DenseMap<Value, CollectionState>;

//===----------------------------------------------------------------------===//
// collection -> pool place + cursor cell
//===----------------------------------------------------------------------===//

struct CollectionLowering : OpRewritePattern<CollectionOp> {
  CollectionLowering(MLIRContext *ctx, StateMap &state)
      : OpRewritePattern(ctx), state(state) {}

  LogicalResult matchAndRewrite(CollectionOp op,
                                PatternRewriter &rewriter) const override {
    Location loc = op.getLoc();
    Type element = op.getElementType();
    MLIRContext *ctx = rewriter.getContext();

    IntegerAttr capAttr = op.getCapacityAttr();
    if (!capAttr)
      return rewriter.notifyMatchFailure(
          op, "array backend requires a folded capacity");
    Type poolTy = ArrayType::get(ctx, capAttr.getInt(), element);
    Value place =
        rewriter.create<VariableOp>(loc, LValueType::get(poolTy)).getResult();
    Type i64 = rewriter.getI64Type();
    Value cursor =
        rewriter.create<memref::AllocaOp>(loc, MemRefType::get({}, i64))
            .getResult();
    Value zero =
        rewriter.create<arith::ConstantOp>(loc, IntegerAttr::get(i64, 0))
            .getResult();
    rewriter.create<memref::StoreOp>(loc, zero, cursor);
    state[place] = {place, cursor};
    rewriter.replaceOp(op, place);
    return success();
  }

  StateMap &state;
};

//===----------------------------------------------------------------------===//
// collection_push -> index
//===----------------------------------------------------------------------===//

struct CollectionPushLowering : OpRewritePattern<CollectionPushOp> {
  CollectionPushLowering(MLIRContext *ctx, StateMap &state)
      : OpRewritePattern(ctx), state(state) {}

  LogicalResult matchAndRewrite(CollectionPushOp op,
                                PatternRewriter &rewriter) const override {
    auto it = state.find(op.getCollection());
    if (it == state.end())
      return failure(); // collection not lowered yet; retry.
    const CollectionState &st = it->second;
    Location loc = op.getLoc();
    Type i64 = rewriter.getI64Type();
    // index = cursor; cursor = cursor + 1. The slot is already the array's
    // default element, so "append" is just a cursor bump.
    Value idx = rewriter.create<memref::LoadOp>(loc, st.cursor).getResult();
    Value one =
        rewriter.create<arith::ConstantOp>(loc, IntegerAttr::get(i64, 1))
            .getResult();
    Value next = rewriter.create<arith::AddIOp>(loc, idx, one).getResult();
    rewriter.create<memref::StoreOp>(loc, next, st.cursor);
    rewriter.replaceOp(op, idx);
    return success();
  }

  StateMap &state;
};

//===----------------------------------------------------------------------===//
// collection_at -> element place
//===----------------------------------------------------------------------===//

struct CollectionAtLowering : OpRewritePattern<CollectionAtOp> {
  CollectionAtLowering(MLIRContext *ctx, StateMap &state)
      : OpRewritePattern(ctx), state(state) {}

  LogicalResult matchAndRewrite(CollectionAtOp op,
                                PatternRewriter &rewriter) const override {
    auto it = state.find(op.getCollection());
    if (it == state.end())
      return failure(); // collection not lowered yet; retry.
    const CollectionState &st = it->second;
    Value element = rewriter
                        .create<SubscriptOp>(op.getLoc(),
                                             op.getResult().getType(), st.place,
                                             op.getIndex())
                        .getResult();
    rewriter.replaceOp(op, element);
    return success();
  }

  StateMap &state;
};

struct EmitRustLowerContainersPass
    : emitrust::impl::EmitRustLowerContainersBase<
          EmitRustLowerContainersPass> {
  using EmitRustLowerContainersBase::EmitRustLowerContainersBase;

  void runOnOperation() override {
    StateMap state;
    RewritePatternSet patterns(&getContext());
    patterns.add<CollectionLowering>(&getContext(), state);
    patterns.add<CollectionPushLowering, CollectionAtLowering>(&getContext(),
                                                               state);

    GreedyRewriteConfig config;
    config.setRegionSimplificationLevel(GreedySimplifyRegionLevel::Disabled);
    if (failed(applyPatternsGreedily(getOperation(), std::move(patterns),
                                     config)))
      signalPassFailure();
  }
};

} // namespace
