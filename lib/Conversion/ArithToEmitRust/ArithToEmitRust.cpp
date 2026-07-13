//===- ArithToEmitRust.cpp - Arith to EmitRust conversion -----------------===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements the conversion from the Arith dialect to the EmitRust dialect:
/// scalar constants, the signed integer and float binary operations, signed
/// and ordered comparisons, and the signed cast operations. Unsigned
/// operations are deliberately left without patterns (MVP boundary): they
/// stay illegal so the conversion fails with the standard diagnostic.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Conversion/ArithToEmitRust.h"

#include "EmitRust/Conversion/EmitRustTypeConverter.h"
#include "EmitRust/EmitRustOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

#include <optional>

#ifdef EMITRUST_ENABLE_PDLL
#include "mlir/Dialect/PDL/IR/PDL.h"
#include "mlir/Dialect/PDLInterp/IR/PDLInterp.h"
#include "mlir/Parser/Parser.h"
#endif // EMITRUST_ENABLE_PDLL

namespace mlir {
namespace emitrust {
#define GEN_PASS_DEF_CONVERTARITHTOEMITRUST
#include "EmitRust/Conversion/Passes.h.inc"
} // namespace emitrust
} // namespace mlir

using namespace mlir;
using namespace mlir::emitrust;

#ifdef EMITRUST_ENABLE_PDLL
#include "ArithToEmitRustPDLLPatterns.h.inc"
#endif // EMITRUST_ENABLE_PDLL

//===----------------------------------------------------------------------===//
// Predicate mapping
//===----------------------------------------------------------------------===//

/// Maps a signed `arith.cmpi` predicate onto the EmitRust comparison
/// predicate, or std::nullopt for the unsupported (unsigned) predicates.
static std::optional<emitrust::CmpPredicate>
convertCmpIPredicate(arith::CmpIPredicate predicate) {
  switch (predicate) {
  case arith::CmpIPredicate::eq:
    return emitrust::CmpPredicate::eq;
  case arith::CmpIPredicate::ne:
    return emitrust::CmpPredicate::ne;
  case arith::CmpIPredicate::slt:
    return emitrust::CmpPredicate::lt;
  case arith::CmpIPredicate::sle:
    return emitrust::CmpPredicate::le;
  case arith::CmpIPredicate::sgt:
    return emitrust::CmpPredicate::gt;
  case arith::CmpIPredicate::sge:
    return emitrust::CmpPredicate::ge;
  default:
    return std::nullopt;
  }
}

/// Maps an ordered `arith.cmpf` predicate onto the EmitRust comparison
/// predicate, or std::nullopt for the unsupported (unordered and
/// always-true/false) predicates.
static std::optional<emitrust::CmpPredicate>
convertCmpFPredicate(arith::CmpFPredicate predicate) {
  switch (predicate) {
  case arith::CmpFPredicate::OEQ:
    return emitrust::CmpPredicate::eq;
  case arith::CmpFPredicate::ONE:
    return emitrust::CmpPredicate::ne;
  case arith::CmpFPredicate::OLT:
    return emitrust::CmpPredicate::lt;
  case arith::CmpFPredicate::OLE:
    return emitrust::CmpPredicate::le;
  case arith::CmpFPredicate::OGT:
    return emitrust::CmpPredicate::gt;
  case arith::CmpFPredicate::OGE:
    return emitrust::CmpPredicate::ge;
  default:
    return std::nullopt;
  }
}

//===----------------------------------------------------------------------===//
// Conversion patterns
//===----------------------------------------------------------------------===//

namespace {

/// Converts scalar `arith.constant` (integer, index, float) into
/// `emitrust.constant`. Non-scalar constants (dense, ...) stay illegal.
struct ArithConstantOpConversion
    : public OpConversionPattern<arith::ConstantOp> {
  using OpConversionPattern<arith::ConstantOp>::OpConversionPattern;

  /// Rewrites the constant, keeping its typed attribute value.
  LogicalResult
  matchAndRewrite(arith::ConstantOp op, OpAdaptor /*adaptor*/,
                  ConversionPatternRewriter &rewriter) const override {
    TypedAttr value = op.getValue();
    if (!isa<IntegerAttr, FloatAttr>(value))
      return rewriter.notifyMatchFailure(
          op, "only scalar integer and float constants are supported");
    Type resultType = getTypeConverter()->convertType(op.getType());
    if (!resultType)
      return rewriter.notifyMatchFailure(op, "result type conversion failed");
    rewriter.replaceOpWithNewOp<emitrust::ConstantOp>(op, resultType, value);
    return success();
  }
};

/// Converts an Arith binary operation into the corresponding EmitRust
/// binary operation with the same operands and (converted) result type.
template <typename ArithOp, typename EmitRustOp>
struct BinaryOpConversion : public OpConversionPattern<ArithOp> {
  using OpConversionPattern<ArithOp>::OpConversionPattern;

  /// Rewrites lhs/rhs into the EmitRust equivalent.
  LogicalResult
  matchAndRewrite(ArithOp op, typename ArithOp::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Type resultType = this->getTypeConverter()->convertType(op.getType());
    if (!resultType)
      return rewriter.notifyMatchFailure(op, "result type conversion failed");
    rewriter.replaceOpWithNewOp<EmitRustOp>(op, resultType, adaptor.getLhs(),
                                            adaptor.getRhs());
    return success();
  }
};

/// Converts `arith.cmpi` with a signed predicate into `emitrust.cmp`.
/// Unsigned predicates have no EmitRust counterpart in the MVP and fail.
struct CmpIOpConversion : public OpConversionPattern<arith::CmpIOp> {
  using OpConversionPattern<arith::CmpIOp>::OpConversionPattern;

  /// Rewrites the comparison, translating the predicate mnemonic.
  LogicalResult
  matchAndRewrite(arith::CmpIOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    std::optional<emitrust::CmpPredicate> predicate =
        convertCmpIPredicate(op.getPredicate());
    if (!predicate)
      return rewriter.notifyMatchFailure(
          op, "unsigned cmpi predicates are not supported");
    Type resultType = getTypeConverter()->convertType(op.getType());
    if (!resultType)
      return rewriter.notifyMatchFailure(op, "result type conversion failed");
    rewriter.replaceOpWithNewOp<emitrust::CmpOp>(
        op, resultType, *predicate, adaptor.getLhs(), adaptor.getRhs());
    return success();
  }
};

/// Converts `arith.cmpf` with an ordered predicate into `emitrust.cmp`.
/// Unordered predicates have no EmitRust counterpart in the MVP and fail.
struct CmpFOpConversion : public OpConversionPattern<arith::CmpFOp> {
  using OpConversionPattern<arith::CmpFOp>::OpConversionPattern;

  /// Rewrites the comparison, translating the predicate mnemonic.
  LogicalResult
  matchAndRewrite(arith::CmpFOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    std::optional<emitrust::CmpPredicate> predicate =
        convertCmpFPredicate(op.getPredicate());
    if (!predicate)
      return rewriter.notifyMatchFailure(
          op, "only the ordered oeq/one/olt/ole/ogt/oge cmpf predicates are "
              "supported");
    Type resultType = getTypeConverter()->convertType(op.getType());
    if (!resultType)
      return rewriter.notifyMatchFailure(op, "result type conversion failed");
    rewriter.replaceOpWithNewOp<emitrust::CmpOp>(
        op, resultType, *predicate, adaptor.getLhs(), adaptor.getRhs());
    return success();
  }
};

/// Converts an Arith cast-like operation (extsi, trunci, sitofp, fptosi,
/// index_cast) into an `emitrust.cast` rendered as a Rust `as` expression.
template <typename ArithOp>
struct CastOpConversion : public OpConversionPattern<ArithOp> {
  using OpConversionPattern<ArithOp>::OpConversionPattern;

  /// Rewrites the cast, keeping the source operand. Casts producing i1 are
  /// left to the dedicated boolean patterns: Rust has no `as bool`.
  LogicalResult
  matchAndRewrite(ArithOp op, typename ArithOp::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    if (op.getType().isInteger(1))
      return rewriter.notifyMatchFailure(
          op, "casts to i1 have no Rust `as` equivalent");
    Type resultType = this->getTypeConverter()->convertType(op.getType());
    if (!resultType)
      return rewriter.notifyMatchFailure(op, "result type conversion failed");
    rewriter.replaceOpWithNewOp<emitrust::CastOp>(op, resultType,
                                                  adaptor.getIn());
    return success();
  }
};

/// Converts `arith.trunci` with an `i1` result into `(v % 2) != 0`, which
/// selects exactly the low bit in two's complement (also for negative odd
/// values, whose Rust remainder is -1). The control-flow lifting pipeline
/// produces this shape when a boolean round-trips through an integer block
/// argument (extui to i32, trunci back to i1 at the scf.condition).
struct TruncIToBoolOpConversion : public OpConversionPattern<arith::TruncIOp> {
  using OpConversionPattern<arith::TruncIOp>::OpConversionPattern;

  /// Rewrites the truncation as a remainder plus a comparison against zero.
  LogicalResult
  matchAndRewrite(arith::TruncIOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    if (!op.getType().isInteger(1))
      return rewriter.notifyMatchFailure(op, "only i1 results are handled");
    Location loc = op.getLoc();
    Type sourceType = adaptor.getIn().getType();
    Value two = emitrust::ConstantOp::create(
        rewriter, loc, sourceType, rewriter.getIntegerAttr(sourceType, 2));
    Value lowBit =
        emitrust::RemOp::create(rewriter, loc, sourceType, adaptor.getIn(), two);
    Value zero = emitrust::ConstantOp::create(
        rewriter, loc, sourceType, rewriter.getIntegerAttr(sourceType, 0));
    rewriter.replaceOpWithNewOp<emitrust::CmpOp>(
        op, rewriter.getI1Type(), emitrust::CmpPredicate::ne, lowBit, zero);
    return success();
  }
};

/// Converts `arith.extui` into `emitrust.cast`, but only for an `i1` source:
/// Rust's `bool as iN` yields exactly 0 or 1, matching the zero-extension.
/// Wider unsigned extensions have no Rust `as` equivalent on signed types and
/// stay illegal (MVP boundary). The control-flow lifting pipeline synthesizes
/// this shape when a boolean feeds an integer multiplexer.
struct ExtUIOpConversion : public OpConversionPattern<arith::ExtUIOp> {
  using OpConversionPattern<arith::ExtUIOp>::OpConversionPattern;

  /// Rewrites the bool-to-integer extension as a cast expression.
  LogicalResult
  matchAndRewrite(arith::ExtUIOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    if (!op.getIn().getType().isInteger(1))
      return rewriter.notifyMatchFailure(
          op, "only i1 sources of extui are supported");
    Type resultType = getTypeConverter()->convertType(op.getType());
    if (!resultType)
      return rewriter.notifyMatchFailure(op, "result type conversion failed");
    rewriter.replaceOpWithNewOp<emitrust::CastOp>(op, resultType,
                                                  adaptor.getIn());
    return success();
  }
};

/// Converts `arith.index_castui` into `emitrust.cast` (`as usize`). The
/// Rust cast sign-extends where index_castui zero-extends, so the two agree
/// exactly for non-negative sources; the only producer in the pipeline is
/// the control-flow lifting pass, whose synthesized block discriminators are
/// small non-negative constants.
struct IndexCastUIOpConversion
    : public OpConversionPattern<arith::IndexCastUIOp> {
  using OpConversionPattern<arith::IndexCastUIOp>::OpConversionPattern;

  /// Rewrites the unsigned index cast as a cast expression.
  LogicalResult
  matchAndRewrite(arith::IndexCastUIOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Type resultType = getTypeConverter()->convertType(op.getType());
    if (!resultType)
      return rewriter.notifyMatchFailure(op, "result type conversion failed");
    rewriter.replaceOpWithNewOp<emitrust::CastOp>(op, resultType,
                                                  adaptor.getIn());
    return success();
  }
};

} // namespace

//===----------------------------------------------------------------------===//
// Pattern population
//===----------------------------------------------------------------------===//

/// Collects the patterns converting Arith operations into EmitRust
/// operations. Unsigned operations deliberately have no patterns.
void mlir::emitrust::populateArithToEmitRustPatterns(
    TypeConverter &typeConverter, RewritePatternSet &patterns) {
  MLIRContext *context = patterns.getContext();
  patterns.add<ArithConstantOpConversion,
               BinaryOpConversion<arith::AddIOp, emitrust::AddOp>,
               BinaryOpConversion<arith::SubIOp, emitrust::SubOp>,
               BinaryOpConversion<arith::MulIOp, emitrust::MulOp>,
               BinaryOpConversion<arith::DivSIOp, emitrust::DivOp>,
               BinaryOpConversion<arith::RemSIOp, emitrust::RemOp>,
               BinaryOpConversion<arith::AddFOp, emitrust::AddOp>,
               BinaryOpConversion<arith::SubFOp, emitrust::SubOp>,
               BinaryOpConversion<arith::MulFOp, emitrust::MulOp>,
               BinaryOpConversion<arith::DivFOp, emitrust::DivOp>,
               CmpIOpConversion, CmpFOpConversion, ExtUIOpConversion,
               TruncIToBoolOpConversion,
               CastOpConversion<arith::ExtSIOp>,
               CastOpConversion<arith::TruncIOp>,
               CastOpConversion<arith::SIToFPOp>,
               CastOpConversion<arith::FPToSIOp>,
               CastOpConversion<arith::IndexCastOp>,
               IndexCastUIOpConversion>(typeConverter, context);
}

//===----------------------------------------------------------------------===//
// Pass definition
//===----------------------------------------------------------------------===//

namespace {

/// The convert-arith-to-emitrust pass: applies the Arith-to-EmitRust
/// patterns under a partial conversion in which the Arith dialect is
/// illegal. When built with EMITRUST_ENABLE_PDLL, the PDLL showcase
/// patterns are registered alongside the C++ ones.
struct ConvertArithToEmitRust
    : public emitrust::impl::ConvertArithToEmitRustBase<
          ConvertArithToEmitRust> {
#ifdef EMITRUST_ENABLE_PDLL
  /// Additionally loads the PDL dialects the compiled PDLL patterns are
  /// interpreted through.
  void getDependentDialects(DialectRegistry &registry) const override {
    Base::getDependentDialects(registry);
    registry.insert<pdl::PDLDialect, pdl_interp::PDLInterpDialect>();
  }
#endif // EMITRUST_ENABLE_PDLL

  /// Runs the partial conversion and fails the pass on leftover Arith ops.
  void runOnOperation() override {
    ConversionTarget target(getContext());
    target.addLegalDialect<EmitRustDialect>();
    target.addIllegalDialect<arith::ArithDialect>();

    TypeConverter typeConverter;
    populateEmitRustTypeConverter(typeConverter);

    RewritePatternSet patterns(&getContext());
    populateArithToEmitRustPatterns(typeConverter, patterns);
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
