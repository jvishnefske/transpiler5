//===- EmitRustRangeInference.cpp - Integer range inference ---------------===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements `InferIntRangeInterface` for the EmitRust operations that
/// carry integer values: constant, literal, enum_raw, cast, cmp, select,
/// and the binary arithmetic operations (add, sub, mul, div, rem, and, or,
/// xor, shl, shr).
///
/// Two dialect-specific rules shape every transfer function here:
///
/// * WRAP semantics. Unsigned EmitRust arithmetic is emitted as Rust
///   `wrapping_*` method calls and signless arithmetic mirrors two's
///   complement machine arithmetic, so all transfer functions delegate to
///   the `mlir::intrange::infer*` helpers with `OverflowFlags::None`
///   (never nsw/nuw): overflow is a defined wrap-around, never poison.
///
/// * Signedness from the type. EmitRust integer types are `ui<N>`
///   (unsigned, rendered `uN`) and signless `i<N>` (rendered as Rust's
///   signed `iN`). Signedness-dependent transfer functions (div, rem, shr,
///   ordered cmp predicates, widening casts) therefore select the unsigned
///   variant exactly when the operand type `isUnsignedInteger()`, with the
///   one exception of `i1`/bool, which zero-extends under Rust `as`.
///
/// Values of non-integer type (floats, lvalues, structs, opaque types) are
/// never constrained: their results are set to the maximal range when the
/// result is an integer whose inputs are unanalyzable, and left untouched
/// otherwise.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/EmitRustOps.h"

#include "mlir/Interfaces/InferIntRangeInterface.h"
#include "mlir/Interfaces/Utils/InferIntRangeCommon.h"

using namespace mlir;
using namespace mlir::emitrust;

/// Returns the integer-range storage width of `type`: the bitwidth for
/// integer types, the index storage width for `index`, and 0 for any
/// non-integer type (which the range domain cannot describe).
static unsigned rangeWidth(Type type) {
  return ConstantIntRanges::getStorageBitwidth(type);
}

/// Returns true if every range in `argRanges` has exactly `width` bits,
/// i.e. the operand lattice values are usable for a `width`-bit transfer
/// function.
static bool allRangesHaveWidth(ArrayRef<ConstantIntRanges> argRanges,
                               unsigned width) {
  return llvm::all_of(argRanges, [width](const ConstantIntRanges &range) {
    return range.umin().getBitWidth() == width;
  });
}

/// Shared transfer-function driver for the binary arithmetic operations:
/// applies `infer` when the common operand/result type is an integer whose
/// operand ranges are well-formed and pins the result to the maximal range
/// otherwise (non-integer types are skipped entirely).
static void inferBinaryOp(Value result, ArrayRef<ConstantIntRanges> argRanges,
                          SetIntRangeFn setResultRanges,
                          function_ref<ConstantIntRanges(
                              ArrayRef<ConstantIntRanges>)> infer) {
  unsigned width = rangeWidth(result.getType());
  if (width == 0)
    return;
  if (!allRangesHaveWidth(argRanges, width)) {
    setResultRanges(result, ConstantIntRanges::maxRange(width));
    return;
  }
  setResultRanges(result, infer(argRanges));
}

//===----------------------------------------------------------------------===//
// ConstantOp
//===----------------------------------------------------------------------===//

void ConstantOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                                   SetIntRangeFn setResultRanges) {
  unsigned width = rangeWidth(getResult().getType());
  if (width == 0)
    return;
  if (auto intAttr = dyn_cast<IntegerAttr>(getValue());
      intAttr && intAttr.getValue().getBitWidth() == width) {
    setResultRanges(getResult(),
                    ConstantIntRanges::constant(intAttr.getValue()));
    return;
  }
  // Opaque constants (e.g. `i64::MAX`) and width mismatches stay at the
  // full range of the result type.
  setResultRanges(getResult(), ConstantIntRanges::maxRange(width));
}

//===----------------------------------------------------------------------===//
// LiteralOp
//===----------------------------------------------------------------------===//

void LiteralOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                                  SetIntRangeFn setResultRanges) {
  // The literal text is opaque; an integer result can hold any value.
  unsigned width = rangeWidth(getResult().getType());
  if (width != 0)
    setResultRanges(getResult(), ConstantIntRanges::maxRange(width));
}

//===----------------------------------------------------------------------===//
// EnumRawOp
//===----------------------------------------------------------------------===//

void EnumRawOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                                  SetIntRangeFn setResultRanges) {
  // The result is an lvalue (a place), not an integer SSA value; there is
  // nothing to constrain in the integer-range domain.
}

//===----------------------------------------------------------------------===//
// CastOp
//===----------------------------------------------------------------------===//

void CastOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                               SetIntRangeFn setResultRanges) {
  Type srcType = getSource().getType();
  Type dstType = getResult().getType();
  unsigned srcWidth = rangeWidth(srcType);
  unsigned dstWidth = rangeWidth(dstType);
  if (dstWidth == 0)
    return;
  const ConstantIntRanges &source = argRanges[0];
  if (srcWidth == 0 || source.umin().getBitWidth() != srcWidth) {
    // Enum-to-integer and other non-integer sources: any value of the
    // result type is possible.
    setResultRanges(getResult(), ConstantIntRanges::maxRange(dstWidth));
    return;
  }
  if (dstWidth == srcWidth) {
    // Same-width `as` casts preserve the bit pattern; both interpretations'
    // bounds remain valid.
    setResultRanges(getResult(), source);
    return;
  }
  if (dstWidth < srcWidth) {
    setResultRanges(getResult(), intrange::truncRange(source, dstWidth));
    return;
  }
  // Widening: Rust `as` zero-extends unsigned and bool sources and
  // sign-extends signed ones; signless `i<N>` renders as the signed `iN`.
  bool zeroExtends = srcType.isUnsignedInteger() || srcType.isInteger(1);
  setResultRanges(getResult(), zeroExtends
                                   ? intrange::extUIRange(source, dstWidth)
                                   : intrange::extSIRange(source, dstWidth));
}

//===----------------------------------------------------------------------===//
// CmpOp
//===----------------------------------------------------------------------===//

/// Maps an EmitRust comparison predicate onto the common-inference
/// predicate space, resolving the ordered predicates to their unsigned
/// variants exactly when `isUnsigned` is set (Rust's `<` on `uN` compares
/// unsigned, on `iN` signed).
static intrange::CmpPredicate mapCmpPredicate(CmpPredicate predicate,
                                              bool isUnsigned) {
  switch (predicate) {
  case CmpPredicate::eq:
    return intrange::CmpPredicate::eq;
  case CmpPredicate::ne:
    return intrange::CmpPredicate::ne;
  case CmpPredicate::lt:
    return isUnsigned ? intrange::CmpPredicate::ult
                      : intrange::CmpPredicate::slt;
  case CmpPredicate::le:
    return isUnsigned ? intrange::CmpPredicate::ule
                      : intrange::CmpPredicate::sle;
  case CmpPredicate::gt:
    return isUnsigned ? intrange::CmpPredicate::ugt
                      : intrange::CmpPredicate::sgt;
  case CmpPredicate::ge:
    return isUnsigned ? intrange::CmpPredicate::uge
                      : intrange::CmpPredicate::sge;
  }
  llvm_unreachable("unknown EmitRust comparison predicate");
}

void CmpOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                              SetIntRangeFn setResultRanges) {
  APInt falseValue = APInt::getZero(1);
  APInt trueValue = APInt::getAllOnes(1);
  Type operandType = getLhs().getType();
  unsigned operandWidth = rangeWidth(operandType);
  // Non-integer operands (floats, enums, fn_ptrs, opaque types) leave the
  // boolean result unrefined at [0, 1].
  if (operandWidth == 0 || !allRangesHaveWidth(argRanges, operandWidth)) {
    setResultRanges(getResult(),
                    ConstantIntRanges::fromUnsigned(falseValue, trueValue));
    return;
  }
  intrange::CmpPredicate predicate =
      mapCmpPredicate(getPredicate(), operandType.isUnsignedInteger());
  std::optional<bool> verdict =
      intrange::evaluatePred(predicate, argRanges[0], argRanges[1]);
  if (verdict.has_value()) {
    setResultRanges(getResult(), ConstantIntRanges::constant(
                                     *verdict ? trueValue : falseValue));
    return;
  }
  setResultRanges(getResult(),
                  ConstantIntRanges::fromUnsigned(falseValue, trueValue));
}

//===----------------------------------------------------------------------===//
// SelectOp
//===----------------------------------------------------------------------===//

void SelectOp::inferResultRangesFromOptional(
    ArrayRef<IntegerValueRange> argRanges, SetIntLatticeFn setResultRanges) {
  // Mirrors arith.select: a statically known condition forwards the taken
  // branch's lattice value, otherwise the result is the branch union.
  std::optional<APInt> condValue =
      argRanges[0].isUninitialized()
          ? std::nullopt
          : argRanges[0].getValue().getConstantValue();
  const IntegerValueRange &trueRange = argRanges[1];
  const IntegerValueRange &falseRange = argRanges[2];
  if (condValue.has_value()) {
    setResultRanges(getResult(), condValue->isZero() ? falseRange : trueRange);
    return;
  }
  setResultRanges(getResult(), IntegerValueRange::join(trueRange, falseRange));
}

//===----------------------------------------------------------------------===//
// Binary arithmetic operations
//===----------------------------------------------------------------------===//

void AddOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                              SetIntRangeFn setResultRanges) {
  inferBinaryOp(getResult(), argRanges, setResultRanges,
                [](ArrayRef<ConstantIntRanges> ranges) {
                  return intrange::inferAdd(ranges,
                                            intrange::OverflowFlags::None);
                });
}

void SubOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                              SetIntRangeFn setResultRanges) {
  inferBinaryOp(getResult(), argRanges, setResultRanges,
                [](ArrayRef<ConstantIntRanges> ranges) {
                  return intrange::inferSub(ranges,
                                            intrange::OverflowFlags::None);
                });
}

void MulOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                              SetIntRangeFn setResultRanges) {
  inferBinaryOp(getResult(), argRanges, setResultRanges,
                [](ArrayRef<ConstantIntRanges> ranges) {
                  return intrange::inferMul(ranges,
                                            intrange::OverflowFlags::None);
                });
}

void DivOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                              SetIntRangeFn setResultRanges) {
  bool isUnsigned = getResult().getType().isUnsignedInteger();
  inferBinaryOp(getResult(), argRanges, setResultRanges,
                [isUnsigned](ArrayRef<ConstantIntRanges> ranges) {
                  return isUnsigned ? intrange::inferDivU(ranges)
                                    : intrange::inferDivS(ranges);
                });
}

void RemOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                              SetIntRangeFn setResultRanges) {
  bool isUnsigned = getResult().getType().isUnsignedInteger();
  inferBinaryOp(getResult(), argRanges, setResultRanges,
                [isUnsigned](ArrayRef<ConstantIntRanges> ranges) {
                  return isUnsigned ? intrange::inferRemU(ranges)
                                    : intrange::inferRemS(ranges);
                });
}

void AndOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                              SetIntRangeFn setResultRanges) {
  inferBinaryOp(getResult(), argRanges, setResultRanges, intrange::inferAnd);
}

void OrOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                             SetIntRangeFn setResultRanges) {
  inferBinaryOp(getResult(), argRanges, setResultRanges, intrange::inferOr);
}

void XorOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                              SetIntRangeFn setResultRanges) {
  inferBinaryOp(getResult(), argRanges, setResultRanges, intrange::inferXor);
}

void ShlOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                              SetIntRangeFn setResultRanges) {
  inferBinaryOp(getResult(), argRanges, setResultRanges,
                [](ArrayRef<ConstantIntRanges> ranges) {
                  return intrange::inferShl(ranges,
                                            intrange::OverflowFlags::None);
                });
}

void ShrOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                              SetIntRangeFn setResultRanges) {
  bool isUnsigned = getResult().getType().isUnsignedInteger();
  inferBinaryOp(getResult(), argRanges, setResultRanges,
                [isUnsigned](ArrayRef<ConstantIntRanges> ranges) {
                  return isUnsigned ? intrange::inferShrU(ranges)
                                    : intrange::inferShrS(ranges);
                });
}
