//===- SCFToEmitRust.h - SCF to EmitRust conversion -------------*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares the conversion from the SCF dialect to the EmitRust dialect,
/// preserving structured control flow: `scf.if` to `emitrust.if`,
/// `scf.while` to `emitrust.loop` with an `emitrust.if` + `emitrust.break`
/// exit, `scf.for` to `emitrust.for`, and `scf.index_switch` to
/// `emitrust.switch`. SSA results are modeled as mutable `emitrust.let`
/// bindings assigned from within the regions.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CONVERSION_SCFTOEMITRUST_H
#define EMITRUST_CONVERSION_SCFTOEMITRUST_H

#include <memory>

namespace mlir {
class Pass;
class RewritePatternSet;
class TypeConverter;

namespace emitrust {

#define GEN_PASS_DECL_CONVERTSCFTOEMITRUST
#include "EmitRust/Conversion/Passes.h.inc"

/// Collects the patterns that convert SCF operations (`scf.if`,
/// `scf.while`, `scf.for`, `scf.index_switch`) into EmitRust structured
/// control flow.
void populateSCFToEmitRustPatterns(TypeConverter &typeConverter,
                                   RewritePatternSet &patterns);

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CONVERSION_SCFTOEMITRUST_H
