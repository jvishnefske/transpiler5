//===- UBToEmitRust.cpp - UB to EmitRust conversion -----------------------===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements the conversion from the UB dialect to the EmitRust dialect.
/// The EmitRust MVP has no notion of undefined values, so `ub.poison`
/// collapses to an `emitrust.constant` holding the deterministic
/// zero/default value of its result type (0, 0.0, false).
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Conversion/UBToEmitRust.h"

#include "EmitRust/Conversion/EmitRustTypeConverter.h"
#include "EmitRust/EmitRustOps.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

#ifdef EMITRUST_ENABLE_PDLL
#include "mlir/Dialect/PDL/IR/PDL.h"
#include "mlir/Dialect/PDLInterp/IR/PDLInterp.h"
#include "mlir/Parser/Parser.h"
#endif // EMITRUST_ENABLE_PDLL

namespace mlir {
namespace emitrust {
#define GEN_PASS_DEF_CONVERTUBTOEMITRUST
#include "EmitRust/Conversion/Passes.h.inc"
} // namespace emitrust
} // namespace mlir

using namespace mlir;
using namespace mlir::emitrust;

#ifdef EMITRUST_ENABLE_PDLL
#include "UBToEmitRustPDLLPatterns.h.inc"
#endif // EMITRUST_ENABLE_PDLL

//===----------------------------------------------------------------------===//
// Conversion patterns
//===----------------------------------------------------------------------===//

namespace {

/// Converts `ub.poison` into an `emitrust.constant` holding the zero/default
/// typed attribute of the (converted) result type. Poison of a type without
/// a scalar default stays illegal and fails the conversion.
struct PoisonOpConversion : public OpConversionPattern<ub::PoisonOp> {
  using OpConversionPattern<ub::PoisonOp>::OpConversionPattern;

  /// Rewrites the poison value as a default-valued constant.
  LogicalResult
  matchAndRewrite(ub::PoisonOp op, OpAdaptor /*adaptor*/,
                  ConversionPatternRewriter &rewriter) const override {
    Type resultType = getTypeConverter()->convertType(op.getType());
    if (!resultType)
      return rewriter.notifyMatchFailure(op, "result type conversion failed");
    Attribute defaultValue = getDefaultValueAttr(resultType);
    if (!defaultValue)
      return rewriter.notifyMatchFailure(
          op, "no default value for the result type");
    rewriter.replaceOpWithNewOp<emitrust::ConstantOp>(op, resultType,
                                                      defaultValue);
    return success();
  }
};

} // namespace

//===----------------------------------------------------------------------===//
// Pattern population
//===----------------------------------------------------------------------===//

/// Collects the patterns converting UB operations into EmitRust operations.
void mlir::emitrust::populateUBToEmitRustPatterns(TypeConverter &typeConverter,
                                                  RewritePatternSet &patterns) {
  patterns.add<PoisonOpConversion>(typeConverter, patterns.getContext());
}

//===----------------------------------------------------------------------===//
// Pass definition
//===----------------------------------------------------------------------===//

namespace {

/// The convert-ub-to-emitrust pass: applies the UB-to-EmitRust patterns
/// under a partial conversion in which the UB dialect is illegal. When
/// built with EMITRUST_ENABLE_PDLL, the PDLL patterns are registered
/// alongside the C++ ones.
struct ConvertUBToEmitRust
    : public emitrust::impl::ConvertUBToEmitRustBase<ConvertUBToEmitRust> {
#ifdef EMITRUST_ENABLE_PDLL
  /// Additionally loads the PDL dialects the compiled PDLL patterns are
  /// interpreted through.
  void getDependentDialects(DialectRegistry &registry) const override {
    Base::getDependentDialects(registry);
    registry.insert<pdl::PDLDialect, pdl_interp::PDLInterpDialect>();
  }
#endif // EMITRUST_ENABLE_PDLL

  /// Runs the partial conversion and fails the pass on leftover UB ops.
  void runOnOperation() override {
    ConversionTarget target(getContext());
    target.addLegalDialect<EmitRustDialect>();
    target.addIllegalDialect<ub::UBDialect>();

    TypeConverter typeConverter;
    populateEmitRustTypeConverter(typeConverter);

    RewritePatternSet patterns(&getContext());
    populateUBToEmitRustPatterns(typeConverter, patterns);
#ifdef EMITRUST_ENABLE_PDLL
    populateGeneratedPDLLPatterns(patterns,
                                  PDLConversionConfig(&typeConverter));
#endif // EMITRUST_ENABLE_PDLL

    if (failed(applyPartialConversion(getOperation(), target,
                                      std::move(patterns))))
      signalPassFailure();
  }
};

} // namespace
