//===- LowerExternalRequirements.h - FR-52 externals trait ------*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares the pass that turns the external-requirement declarations the C
/// importer marked (`emitrust.external_requirement`) into one
/// `emitrust.trait_def` plus a type parameter on the transitive closure of
/// their callers.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CONVERSION_LOWEREXTERNALREQUIREMENTS_H
#define EMITRUST_CONVERSION_LOWEREXTERNALREQUIREMENTS_H

#include <memory>

namespace mlir {
class Pass;

namespace emitrust {

#define GEN_PASS_DECL_EMITRUSTLOWEREXTERNALREQUIREMENTS
#include "EmitRust/Conversion/Passes.h.inc"

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CONVERSION_LOWEREXTERNALREQUIREMENTS_H
