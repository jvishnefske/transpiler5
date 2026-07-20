//===- FuncToEmitRust.cpp - Func to EmitRust conversion -------------------===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements the conversion from the Func dialect to the EmitRust dialect:
/// `func.func` becomes `emitrust.func` (signature through the type
/// converter, body region inlined), `func.return` becomes
/// `emitrust.return`, and `func.call` becomes `emitrust.call_opaque` with
/// the callee symbol name as the opaque string callee. Functions and calls
/// with more than one result are not convertible.
///
/// The owner-struct surface (Phase 4 of pointer support) is materialized
/// here: a `func.func` carrying the `emitrust.method_of` string attribute
/// is placed inside a lazily created `emitrust.impl` for the named struct
/// (one impl per owner; the attribute is stripped), and a `func.call`
/// carrying the `emitrust.method_call` unit attribute — whose first operand
/// is an `emitrust.addr_of mut` of the owner place — is rewritten into an
/// `emitrust.method_call` on that place, erasing the dead borrow.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Conversion/FuncToEmitRust.h"

#include "EmitRust/Conversion/EmitRustTypeConverter.h"
#include "EmitRust/EmitRustDialect.h"
#include "EmitRust/EmitRustOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

namespace mlir {
namespace emitrust {
#define GEN_PASS_DEF_CONVERTFUNCTOEMITRUST
#include "EmitRust/Conversion/Passes.h.inc"
} // namespace emitrust
} // namespace mlir

using namespace mlir;
using namespace mlir::emitrust;

//===----------------------------------------------------------------------===//
// Conversion patterns
//===----------------------------------------------------------------------===//

namespace {

/// Converts `func.call` into `emitrust.call_opaque` with the callee symbol
/// name as the opaque string callee. A call tagged `emitrust.method_call`
/// by the importer becomes an `emitrust.method_call` on the owner place
/// instead: its first operand must be the `emitrust.addr_of mut` borrow of
/// the receiver, which is consumed and erased so that no materialized
/// borrow survives the conversion.
struct CallOpConversion : public OpConversionPattern<func::CallOp> {
  using OpConversionPattern<func::CallOp>::OpConversionPattern;

  /// Rewrites the direct call as an opaque call or, for method-tagged
  /// calls, as a method call on the borrowed receiver place.
  LogicalResult
  matchAndRewrite(func::CallOp callOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    if (callOp.getNumResults() > 1)
      return rewriter.notifyMatchFailure(
          callOp, "only calls with zero or one result can be converted");

    SmallVector<Type> resultTypes;
    if (failed(getTypeConverter()->convertTypes(callOp.getResultTypes(),
                                                resultTypes)))
      return rewriter.notifyMatchFailure(callOp,
                                         "result type conversion failed");

    if (callOp->hasAttr(emitrust::kMethodCallAttrName))
      return rewriteMethodCall(callOp, adaptor, resultTypes, rewriter);

    rewriter.replaceOpWithNewOp<emitrust::CallOpaqueOp>(
        callOp, resultTypes, callOp.getCallee(), /*args=*/ArrayAttr(),
        adaptor.getOperands());
    return success();
  }

private:
  /// Rewrites a method-tagged call: peels the receiver borrow off the first
  /// operand and calls the callee's symbol name as a Rust method on the
  /// borrowed place. The receiver place may itself be a dereference lvalue
  /// (a sibling method calling through its own `self`), which renders as
  /// `(*self).method(...)`. Any other first-operand shape is a match
  /// failure, which fails the pass loudly on the leftover `func.call`.
  /// W2.2: the borrow may be mutable OR shared (`emitrust.addr_of` without
  /// `mut`) — a call to a const (`&self`) method borrows the receiver
  /// immutably; `emitrust.method_call` itself is receiver-mutability-
  /// agnostic (Rust's auto-ref infers `&`/`&mut` from the resolved method's
  /// own signature), so only this pattern's shape check needs relaxing.
  LogicalResult rewriteMethodCall(func::CallOp callOp, OpAdaptor adaptor,
                                  ArrayRef<Type> resultTypes,
                                  ConversionPatternRewriter &rewriter) const {
    if (adaptor.getOperands().empty())
      return rewriter.notifyMatchFailure(
          callOp, "method call must have a receiver operand");
    auto addrOf = adaptor.getOperands()
                      .front()
                      .getDefiningOp<emitrust::AddrOfOp>();
    if (!addrOf)
      return rewriter.notifyMatchFailure(
          callOp,
          "method call receiver must be produced by emitrust.addr_of");
    if (!addrOf->hasOneUse())
      return rewriter.notifyMatchFailure(
          callOp, "method call receiver borrow must have a single use");

    rewriter.replaceOpWithNewOp<emitrust::MethodCallOp>(
        callOp, resultTypes, addrOf.getOperand(),
        rewriter.getStringAttr(callOp.getCallee()),
        adaptor.getOperands().drop_front());
    rewriter.eraseOp(addrOf);
    return success();
  }
};

/// Converts `func.func` into `emitrust.func`, converting the signature via
/// the type converter and inlining the body region. A function tagged
/// `emitrust.method_of` is created inside a lazily materialized
/// `emitrust.impl` for the named owner struct (the tag is stripped); all
/// other functions are created in place.
struct FuncOpConversion : public OpConversionPattern<func::FuncOp> {
  using OpConversionPattern<func::FuncOp>::OpConversionPattern;

  /// Returns the module-level `emitrust.impl` for `structName`, creating it
  /// (with its single empty block) at the end of the module on first use.
  static emitrust::ImplOp getOrCreateImpl(ModuleOp module, Location loc,
                                          StringAttr structName,
                                          ConversionPatternRewriter &rewriter) {
    for (auto implOp : module.getOps<emitrust::ImplOp>())
      if (implOp.getStructName() == structName.getValue())
        return implOp;
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPointToEnd(module.getBody());
    auto implOp = rewriter.create<emitrust::ImplOp>(loc, structName);
    rewriter.createBlock(&implOp.getBody());
    return implOp;
  }

  /// Rewrites the function, moving its body into the new operation.
  LogicalResult
  matchAndRewrite(func::FuncOp funcOp, OpAdaptor /*adaptor*/,
                  ConversionPatternRewriter &rewriter) const override {
    FunctionType fnType = funcOp.getFunctionType();

    if (fnType.getNumResults() > 1)
      return rewriter.notifyMatchFailure(
          funcOp, "only functions with zero or one result can be converted");

    TypeConverter::SignatureConversion signatureConverter(
        fnType.getNumInputs());
    for (const auto &argType : llvm::enumerate(fnType.getInputs())) {
      Type convertedType = getTypeConverter()->convertType(argType.value());
      if (!convertedType)
        return rewriter.notifyMatchFailure(funcOp,
                                           "argument type conversion failed");
      signatureConverter.addInputs(argType.index(), convertedType);
    }

    Type resultType;
    if (fnType.getNumResults() == 1) {
      resultType = getTypeConverter()->convertType(fnType.getResult(0));
      if (!resultType)
        return rewriter.notifyMatchFailure(funcOp,
                                           "result type conversion failed");
    }

    // Create the converted `emitrust.func` op: inside the owner's impl for
    // a method-tagged function, in place otherwise.
    OpBuilder::InsertionGuard guard(rewriter);
    auto methodOf =
        funcOp->getAttrOfType<StringAttr>(emitrust::kMethodOfAttrName);
    if (methodOf) {
      auto module = funcOp->getParentOfType<ModuleOp>();
      emitrust::ImplOp implOp =
          getOrCreateImpl(module, funcOp.getLoc(), methodOf, rewriter);
      rewriter.setInsertionPointToEnd(&implOp.getBody().front());
    }
    auto newFuncOp = rewriter.create<emitrust::FuncOp>(
        funcOp.getLoc(), funcOp.getName(),
        FunctionType::get(rewriter.getContext(),
                          signatureConverter.getConvertedTypes(),
                          resultType ? TypeRange(resultType) : TypeRange()));

    // Copy over all discardable attributes other than the symbol name, the
    // function type, and the consumed method placement tag.
    for (const NamedAttribute &namedAttr : funcOp->getAttrs()) {
      if (namedAttr.getName() != funcOp.getFunctionTypeAttrName() &&
          namedAttr.getName() != SymbolTable::getSymbolAttrName() &&
          namedAttr.getName() != emitrust::kMethodOfAttrName)
        newFuncOp->setAttr(namedAttr.getName(), namedAttr.getValue());
    }

    if (!funcOp.isDeclaration()) {
      rewriter.inlineRegionBefore(funcOp.getBody(), newFuncOp.getBody(),
                                  newFuncOp.end());
      if (failed(rewriter.convertRegionTypes(
              &newFuncOp.getBody(), *getTypeConverter(), &signatureConverter)))
        return rewriter.notifyMatchFailure(funcOp,
                                           "region types conversion failed");
    }
    rewriter.eraseOp(funcOp);
    return success();
  }
};

/// Converts `func.return` into `emitrust.return` with the same (remapped)
/// operand, if any.
struct ReturnOpConversion : public OpConversionPattern<func::ReturnOp> {
  using OpConversionPattern<func::ReturnOp>::OpConversionPattern;

  /// Rewrites the return terminator.
  LogicalResult
  matchAndRewrite(func::ReturnOp returnOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    if (returnOp.getNumOperands() > 1)
      return rewriter.notifyMatchFailure(
          returnOp, "only zero or one operand is supported");

    rewriter.replaceOpWithNewOp<emitrust::ReturnOp>(
        returnOp,
        returnOp.getNumOperands() ? adaptor.getOperands()[0] : Value());
    return success();
  }
};

} // namespace

//===----------------------------------------------------------------------===//
// Pattern population
//===----------------------------------------------------------------------===//

/// Collects the patterns converting Func operations into EmitRust
/// operations.
void mlir::emitrust::populateFuncToEmitRustPatterns(
    TypeConverter &typeConverter, RewritePatternSet &patterns) {
  patterns.add<CallOpConversion, FuncOpConversion, ReturnOpConversion>(
      typeConverter, patterns.getContext());
}

//===----------------------------------------------------------------------===//
// Pass definition
//===----------------------------------------------------------------------===//

namespace {

/// The convert-func-to-emitrust pass: applies the Func-to-EmitRust patterns
/// under a partial conversion in which the Func dialect is illegal.
struct ConvertFuncToEmitRust
    : public emitrust::impl::ConvertFuncToEmitRustBase<ConvertFuncToEmitRust> {
  /// Runs the partial conversion and fails the pass on leftover Func ops.
  void runOnOperation() override {
    ConversionTarget target(getContext());
    target.addLegalDialect<EmitRustDialect>();
    target.addIllegalDialect<func::FuncDialect>();

    TypeConverter typeConverter;
    populateEmitRustTypeConverter(typeConverter);

    RewritePatternSet patterns(&getContext());
    populateFuncToEmitRustPatterns(typeConverter, patterns);

    if (failed(applyPartialConversion(getOperation(), target,
                                      std::move(patterns))))
      signalPassFailure();
  }
};

} // namespace
