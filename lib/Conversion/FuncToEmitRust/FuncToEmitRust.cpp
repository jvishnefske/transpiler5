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
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Conversion/FuncToEmitRust.h"

#include "EmitRust/Conversion/EmitRustTypeConverter.h"
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
/// name as the opaque string callee.
struct CallOpConversion : public OpConversionPattern<func::CallOp> {
  using OpConversionPattern<func::CallOp>::OpConversionPattern;

  /// Rewrites the direct call as an opaque call.
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

    rewriter.replaceOpWithNewOp<emitrust::CallOpaqueOp>(
        callOp, resultTypes, callOp.getCallee(), /*args=*/ArrayAttr(),
        adaptor.getOperands());
    return success();
  }
};

/// Converts `func.func` into `emitrust.func`, converting the signature via
/// the type converter and inlining the body region.
struct FuncOpConversion : public OpConversionPattern<func::FuncOp> {
  using OpConversionPattern<func::FuncOp>::OpConversionPattern;

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

    // Create the converted `emitrust.func` op.
    auto newFuncOp = rewriter.create<emitrust::FuncOp>(
        funcOp.getLoc(), funcOp.getName(),
        FunctionType::get(rewriter.getContext(),
                          signatureConverter.getConvertedTypes(),
                          resultType ? TypeRange(resultType) : TypeRange()));

    // Copy over all discardable attributes other than the symbol name and
    // the function type.
    for (const NamedAttribute &namedAttr : funcOp->getAttrs()) {
      if (namedAttr.getName() != funcOp.getFunctionTypeAttrName() &&
          namedAttr.getName() != SymbolTable::getSymbolAttrName())
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
