//===- Passes.h - EmitRust conversion passes --------------------*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares the registration entry point for all EmitRust conversion passes.
/// Including this header pulls in every per-conversion header; calling
/// `mlir::emitrust::registerEmitRustConversionPasses()` registers the
/// convert-ub-to-emitrust, convert-arith-to-emitrust,
/// convert-func-to-emitrust, convert-scf-to-emitrust,
/// convert-to-emitrust, emitrust-range-refinement-check, and
/// emitrust-value-identity-check passes with the global pass registry.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CONVERSION_PASSES_H
#define EMITRUST_CONVERSION_PASSES_H

#include "EmitRust/Conversion/ArithToEmitRust.h"
#include "EmitRust/Conversion/ConvertToEmitRust.h"
#include "EmitRust/Conversion/FuncToEmitRust.h"
#include "EmitRust/Conversion/RangeRefinementCheck.h"
#include "EmitRust/Conversion/SCFToEmitRust.h"
#include "EmitRust/Conversion/UBToEmitRust.h"
#include "EmitRust/Conversion/ValueIdentityCheck.h"

#include "mlir/Pass/PassRegistry.h"

namespace mlir {
namespace emitrust {

/// Generates registerEmitRustConversionPasses() and the per-pass
/// registration functions.
#define GEN_PASS_REGISTRATION
#include "EmitRust/Conversion/Passes.h.inc"

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CONVERSION_PASSES_H
