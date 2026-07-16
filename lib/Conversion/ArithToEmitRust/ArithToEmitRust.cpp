//===- ArithToEmitRust.cpp - Arith to EmitRust conversion -----------------===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements the conversion from the Arith dialect to the EmitRust dialect:
/// scalar constants, the signed integer and float binary operations, the
/// bitwise and shift operations, the comparisons with a Rust operator
/// equivalent (signed integer predicates; unsigned integer predicates on
/// unsigned operand types only; the ordered oeq/olt/ole/ogt/oge and the
/// unordered une float predicates), the scalar select, and the signed cast
/// operations. The unsigned-semantics operations (divui, remui, shrui, and
/// the unsigned cmpi predicates) only convert when their type is an
/// unsigned IntegerType, whose Rust rendering (u8..u64) has exactly the
/// unsigned semantics; on a signless type they stay illegal, because the
/// signless Rust rendering (i8..i64) would execute the SIGNED operation, a
/// silent miscompile. (The Arith verifier itself currently constrains these
/// operations to signless types, so the unsigned-accepting direction is a
/// forward-compatibility boundary; the reject direction is what protects
/// today's pipeline.)
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

/// Returns true if `type` is an unsigned IntegerType (ui8..ui64), whose
/// Rust rendering (u8..u64) carries unsigned operator semantics.
static bool isUnsignedIntegerType(Type type) {
  auto intType = dyn_cast<IntegerType>(type);
  return intType && intType.isUnsigned();
}

/// Maps an `arith.cmpi` predicate onto the EmitRust comparison predicate
/// given the operand `type`, or std::nullopt when the pairing has no
/// correct Rust operator. eq/ne map on any type. The signed slt/sle/sgt/sge
/// map as before. The unsigned ult/ule/ugt/uge map ONLY when the operand
/// type is an unsigned IntegerType: Rust's `<` on uN is the unsigned
/// comparison, but on a signless type (rendered iN) it would be the SIGNED
/// comparison — a silent miscompile — so signless operands with unsigned
/// predicates stay illegal.
static std::optional<emitrust::CmpPredicate>
convertCmpIPredicate(arith::CmpIPredicate predicate, Type type) {
  bool isUnsigned = isUnsignedIntegerType(type);
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
  case arith::CmpIPredicate::ult:
    return isUnsigned ? std::optional(emitrust::CmpPredicate::lt)
                      : std::nullopt;
  case arith::CmpIPredicate::ule:
    return isUnsigned ? std::optional(emitrust::CmpPredicate::le)
                      : std::nullopt;
  case arith::CmpIPredicate::ugt:
    return isUnsigned ? std::optional(emitrust::CmpPredicate::gt)
                      : std::nullopt;
  case arith::CmpIPredicate::uge:
    return isUnsigned ? std::optional(emitrust::CmpPredicate::ge)
                      : std::nullopt;
  }
  return std::nullopt;
}

/// Maps an `arith.cmpf` predicate onto the EmitRust comparison predicate,
/// or std::nullopt for the unsupported predicates.
///
/// Rust's float comparison operators are exactly IEEE 754: `==`, `<`, `<=`,
/// `>`, `>=` are the *ordered* comparisons (false when either operand is
/// NaN), matching oeq/olt/ole/ogt/oge; `!=` is the *unordered-or-unequal*
/// comparison (true when either operand is NaN), matching une — which is
/// also exactly what the C importer emits for `!=` and float truthiness.
/// `one` (ordered-and-unequal) deliberately has no mapping: rendering it as
/// Rust `!=` would be wrong for NaN inputs (one is false, `!=` is true),
/// and nothing in the pipeline produces it. Every other unordered predicate
/// (ueq/ugt/uge/ult/ule/uno/ord) equally stays illegal.
static std::optional<emitrust::CmpPredicate>
convertCmpFPredicate(arith::CmpFPredicate predicate) {
  switch (predicate) {
  case arith::CmpFPredicate::OEQ:
    return emitrust::CmpPredicate::eq;
  case arith::CmpFPredicate::UNE:
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

/// Converts `arith.cmpi` into `emitrust.cmp`: the signed predicates map on
/// any type, the unsigned predicates ONLY on unsigned IntegerType operands
/// (Rust's comparison operators on uN are the unsigned comparisons; on the
/// signless rendering iN they would be signed, a silent miscompile).
struct CmpIOpConversion : public OpConversionPattern<arith::CmpIOp> {
  using OpConversionPattern<arith::CmpIOp>::OpConversionPattern;

  /// Rewrites the comparison, translating the predicate mnemonic.
  LogicalResult
  matchAndRewrite(arith::CmpIOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    std::optional<emitrust::CmpPredicate> predicate =
        convertCmpIPredicate(op.getPredicate(), op.getLhs().getType());
    if (!predicate)
      return rewriter.notifyMatchFailure(
          op, "unsigned cmpi predicates are only supported on unsigned "
              "integer types");
    Type resultType = getTypeConverter()->convertType(op.getType());
    if (!resultType)
      return rewriter.notifyMatchFailure(op, "result type conversion failed");
    rewriter.replaceOpWithNewOp<emitrust::CmpOp>(
        op, resultType, *predicate, adaptor.getLhs(), adaptor.getRhs());
    return success();
  }
};

/// Converts an unsigned-semantics Arith binary operation (divui, remui,
/// shrui) into the corresponding EmitRust operation ONLY when the type is
/// an unsigned IntegerType: the Rust rendering uN then has exactly the
/// unsigned semantics (`/` and `%` truncate toward zero, `>>` is a logical
/// shift). On a signless type — rendered iN — the same operators execute
/// the SIGNED operation, so signless operands stay illegal instead of
/// silently miscompiling.
template <typename ArithOp, typename EmitRustOp>
struct UnsignedBinaryOpConversion : public OpConversionPattern<ArithOp> {
  using OpConversionPattern<ArithOp>::OpConversionPattern;

  /// Rewrites lhs/rhs into the EmitRust equivalent when the guard holds.
  LogicalResult
  matchAndRewrite(ArithOp op, typename ArithOp::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    if (!isUnsignedIntegerType(op.getType()))
      return rewriter.notifyMatchFailure(
          op, "unsigned arith operations are only supported on unsigned "
              "integer types");
    Type resultType = this->getTypeConverter()->convertType(op.getType());
    if (!resultType)
      return rewriter.notifyMatchFailure(op, "result type conversion failed");
    rewriter.replaceOpWithNewOp<EmitRustOp>(op, resultType, adaptor.getLhs(),
                                            adaptor.getRhs());
    return success();
  }
};

/// Converts `arith.shrsi` into `emitrust.shr` ONLY when the type is a
/// signless or signed IntegerType: Rust's `>>` on the rendering iN is the
/// arithmetic (sign-extending) shift, exactly shrsi. On an unsigned type
/// (rendered uN) or on index (rendered usize) the same operator is the
/// LOGICAL shift, so those types stay illegal instead of silently
/// miscompiling negative left operands.
struct ShRSIOpConversion : public OpConversionPattern<arith::ShRSIOp> {
  using OpConversionPattern<arith::ShRSIOp>::OpConversionPattern;

  /// Rewrites lhs/rhs into `emitrust.shr` when the guard holds.
  LogicalResult
  matchAndRewrite(arith::ShRSIOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto intType = dyn_cast<IntegerType>(op.getType());
    if (!intType || intType.isUnsigned())
      return rewriter.notifyMatchFailure(
          op, "arith.shrsi is only supported on signless or signed integer "
              "types");
    Type resultType = getTypeConverter()->convertType(op.getType());
    if (!resultType)
      return rewriter.notifyMatchFailure(op, "result type conversion failed");
    rewriter.replaceOpWithNewOp<emitrust::ShrOp>(op, resultType,
                                                 adaptor.getLhs(),
                                                 adaptor.getRhs());
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
          op, "only the oeq/olt/ole/ogt/oge/une cmpf predicates are "
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
/// extf, truncf, index_cast) into an `emitrust.cast` rendered as a Rust
/// `as` expression.
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

/// Converts `arith.index_castui` into `emitrust.cast` operations that
/// preserve its zero-extension semantics. A single Rust `as usize` cast
/// SIGN-extends a signed iN source, so an iN source (N in 8/16/32/64) hops
/// through the unsigned type of the same width first: `v as uN as usize`
/// zero-extends exactly. This matters because the control-flow lifting pass
/// feeds user switch scrutinees through this cast while zero-extending the
/// case values; sign-extending the scrutinee would make every negative case
/// label unreachable. An `i1` source keeps the single cast (Rust `bool as
/// usize` is exactly 0 or 1), as does an `index` source (a `usize as iN`
/// truncation is bit-exact regardless of extension).
struct IndexCastUIOpConversion
    : public OpConversionPattern<arith::IndexCastUIOp> {
  using OpConversionPattern<arith::IndexCastUIOp>::OpConversionPattern;

  /// Rewrites the unsigned index cast as one or two cast expressions.
  LogicalResult
  matchAndRewrite(arith::IndexCastUIOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Type resultType = getTypeConverter()->convertType(op.getType());
    if (!resultType)
      return rewriter.notifyMatchFailure(op, "result type conversion failed");
    Value source = adaptor.getIn();
    if (auto intType = dyn_cast<IntegerType>(source.getType());
        intType && intType.getWidth() > 1) {
      unsigned width = intType.getWidth();
      if (width != 8 && width != 16 && width != 32 && width != 64)
        return rewriter.notifyMatchFailure(
            op, "source width has no Rust integer type");
      Type unsignedType = IntegerType::get(rewriter.getContext(), width,
                                           IntegerType::Unsigned);
      source = emitrust::CastOp::create(rewriter, op.getLoc(), unsignedType,
                                        source);
    }
    rewriter.replaceOpWithNewOp<emitrust::CastOp>(op, resultType, source);
    return success();
  }
};

/// Converts `arith.select` into `emitrust.select`, rendered as a Rust
/// `if cond { a } else { b }` expression. Only the scalar form (an `i1`
/// condition) is supported; shaped selects have no EmitRust counterpart and
/// their result types fail the type converter anyway.
struct SelectOpConversion : public OpConversionPattern<arith::SelectOp> {
  using OpConversionPattern<arith::SelectOp>::OpConversionPattern;

  /// Rewrites the select, keeping the condition and both values.
  LogicalResult
  matchAndRewrite(arith::SelectOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    if (!op.getCondition().getType().isInteger(1))
      return rewriter.notifyMatchFailure(
          op, "only scalar i1 select conditions are supported");
    Type resultType = getTypeConverter()->convertType(op.getType());
    if (!resultType)
      return rewriter.notifyMatchFailure(op, "result type conversion failed");
    rewriter.replaceOpWithNewOp<emitrust::SelectOp>(
        op, resultType, adaptor.getCondition(), adaptor.getTrueValue(),
        adaptor.getFalseValue());
    return success();
  }
};

} // namespace

//===----------------------------------------------------------------------===//
// Pattern population
//===----------------------------------------------------------------------===//

/// Collects the patterns converting Arith operations into EmitRust
/// operations. The unsigned-semantics operations only convert on unsigned
/// IntegerType; on signless types they deliberately stay illegal.
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
               BinaryOpConversion<arith::AndIOp, emitrust::AndOp>,
               BinaryOpConversion<arith::OrIOp, emitrust::OrOp>,
               BinaryOpConversion<arith::XOrIOp, emitrust::XorOp>,
               BinaryOpConversion<arith::ShLIOp, emitrust::ShlOp>,
               ShRSIOpConversion,
               UnsignedBinaryOpConversion<arith::ShRUIOp, emitrust::ShrOp>,
               UnsignedBinaryOpConversion<arith::DivUIOp, emitrust::DivOp>,
               UnsignedBinaryOpConversion<arith::RemUIOp, emitrust::RemOp>,
               CmpIOpConversion, CmpFOpConversion, ExtUIOpConversion,
               TruncIToBoolOpConversion, SelectOpConversion,
               CastOpConversion<arith::ExtSIOp>,
               CastOpConversion<arith::TruncIOp>,
               CastOpConversion<arith::SIToFPOp>,
               CastOpConversion<arith::FPToSIOp>,
               CastOpConversion<arith::ExtFOp>,
               CastOpConversion<arith::TruncFOp>,
               CastOpConversion<arith::IndexCastOp>,
               IndexCastUIOpConversion>(typeConverter, context);
}

//===----------------------------------------------------------------------===//
// Pass definition
//===----------------------------------------------------------------------===//

namespace {

/// The convert-arith-to-emitrust pass: applies the Arith-to-EmitRust
/// patterns under a partial conversion in which the Arith dialect is
/// illegal. When built with EMITRUST_ENABLE_PDLL, the duplicate PDLL
/// patterns (the full declaratively expressible inventory) are registered
/// alongside the C++ ones.
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
