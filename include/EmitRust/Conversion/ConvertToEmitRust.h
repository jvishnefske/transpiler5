//===- ConvertToEmitRust.h - Composite conversion to EmitRust ---*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares the composite convert-to-emitrust pass that populates the ub,
/// arith, func, and scf pattern sets and applies them under a single
/// partial conversion. Any operation from the arith, cf, func, memref,
/// scf, or ub dialects that survives the conversion fails the pass.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CONVERSION_CONVERTTOEMITRUST_H
#define EMITRUST_CONVERSION_CONVERTTOEMITRUST_H

#include <memory>

namespace mlir {
class Pass;

namespace emitrust {

#define GEN_PASS_DECL_CONVERTTOEMITRUST
#include "EmitRust/Conversion/Passes.h.inc"

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CONVERSION_CONVERTTOEMITRUST_H
