//===- FuncToEmitRust.h - Func to EmitRust conversion -----------*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares the conversion from the Func dialect to the EmitRust dialect:
/// `func.func` to `emitrust.func`, `func.return` to `emitrust.return`, and
/// `func.call` to `emitrust.call_opaque` with the callee symbol name as the
/// opaque string callee.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CONVERSION_FUNCTOEMITRUST_H
#define EMITRUST_CONVERSION_FUNCTOEMITRUST_H

#include <memory>

namespace mlir {
class Pass;
class RewritePatternSet;
class TypeConverter;

namespace emitrust {

#define GEN_PASS_DECL_CONVERTFUNCTOEMITRUST
#include "EmitRust/Conversion/Passes.h.inc"

/// Collects the patterns that convert Func operations into EmitRust
/// operations. Functions and calls with more than one result are not
/// convertible and fail the surrounding conversion.
void populateFuncToEmitRustPatterns(TypeConverter &typeConverter,
                                    RewritePatternSet &patterns);

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CONVERSION_FUNCTOEMITRUST_H
