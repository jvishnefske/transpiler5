//===- UBToEmitRust.h - UB to EmitRust conversion ---------------*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares the conversion from the UB dialect to the EmitRust dialect:
/// `ub.poison` collapses to an `emitrust.constant` holding the zero/default
/// value of its result type.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CONVERSION_UBTOEMITRUST_H
#define EMITRUST_CONVERSION_UBTOEMITRUST_H

#include <memory>

namespace mlir {
class Pass;
class RewritePatternSet;
class TypeConverter;

namespace emitrust {

#define GEN_PASS_DECL_CONVERTUBTOEMITRUST
#include "EmitRust/Conversion/Passes.h.inc"

/// Collects the patterns that convert UB operations into EmitRust
/// operations: `ub.poison` becomes an `emitrust.constant` with the
/// zero/default typed attribute of the result type.
void populateUBToEmitRustPatterns(TypeConverter &typeConverter,
                                  RewritePatternSet &patterns);

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CONVERSION_UBTOEMITRUST_H
