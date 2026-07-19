//===- RangeRefinementCheck.h - Differential range checker ------*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares the emitrust-range-refinement-check pass: a differential
/// abstract interpretation that re-derives integer value ranges before and
/// after the convert-to-emitrust pipeline and reports a hard error when the
/// pre- and post-conversion ranges of one observable value (a
/// `emitrust.call_opaque "print!"` operand or a function return operand)
/// are disjoint under both the signed and the unsigned interpretation.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CONVERSION_RANGEREFINEMENTCHECK_H
#define EMITRUST_CONVERSION_RANGEREFINEMENTCHECK_H

#include <memory>

namespace mlir {
class Pass;

namespace emitrust {

#define GEN_PASS_DECL_EMITRUSTRANGEREFINEMENTCHECK
#include "EmitRust/Conversion/Passes.h.inc"

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CONVERSION_RANGEREFINEMENTCHECK_H
