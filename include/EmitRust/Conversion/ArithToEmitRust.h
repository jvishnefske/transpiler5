//===- ArithToEmitRust.h - Arith to EmitRust conversion ---------*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares the conversion from the Arith dialect to the EmitRust dialect:
/// scalar constants, signed integer and float arithmetic, signed/ordered
/// comparisons, and the signed cast operations. Unsigned operations are
/// deliberately unsupported in the MVP and stay illegal.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CONVERSION_ARITHTOEMITRUST_H
#define EMITRUST_CONVERSION_ARITHTOEMITRUST_H

#include <memory>

namespace mlir {
class Pass;
class RewritePatternSet;
class TypeConverter;

namespace emitrust {

#define GEN_PASS_DECL_CONVERTARITHTOEMITRUST
#include "EmitRust/Conversion/Passes.h.inc"

/// Collects the patterns that convert Arith operations into EmitRust
/// operations: `arith.constant` to `emitrust.constant`; the signed integer
/// and float binary operations to `emitrust.add`/`sub`/`mul`/`div`/`rem`;
/// signed `arith.cmpi` and ordered `arith.cmpf` predicates to
/// `emitrust.cmp`; and `extsi`/`trunci`/`sitofp`/`fptosi`/`index_cast` to
/// `emitrust.cast`.
void populateArithToEmitRustPatterns(TypeConverter &typeConverter,
                                     RewritePatternSet &patterns);

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CONVERSION_ARITHTOEMITRUST_H
