//===- ValueIdentityCheck.h - Differential value-identity checker -*- C++ -*-//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares the emitrust-value-identity-check pass: a differential concrete
/// interpretation that runs every parameterless function before and after
/// the convert-to-emitrust pipeline and reports a hard error when the two
/// runs observe different values (a `emitrust.call_opaque "print!"` operand
/// or a function return operand) in execution order. Unsupported coverage
/// soft-skips the entry point; only genuine value divergence fails.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CONVERSION_VALUEIDENTITYCHECK_H
#define EMITRUST_CONVERSION_VALUEIDENTITYCHECK_H

#include <memory>

namespace mlir {
class Pass;

namespace emitrust {

#define GEN_PASS_DECL_EMITRUSTVALUEIDENTITYCHECK
#include "EmitRust/Conversion/Passes.h.inc"

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CONVERSION_VALUEIDENTITYCHECK_H
