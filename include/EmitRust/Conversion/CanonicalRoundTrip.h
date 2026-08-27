//===- CanonicalRoundTrip.h - Canonical-form round-trip check ---*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares the emitrust-canonical-roundtrip pass: an OPT-IN, byte-inert
/// verification stage that round-trips the front end's module through MLIR's
/// canonical (generic) textual form and reports a hard error if the module
/// does not survive it unchanged.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CONVERSION_CANONICALROUNDTRIP_H
#define EMITRUST_CONVERSION_CANONICALROUNDTRIP_H

#include <memory>

namespace mlir {
class Pass;

namespace emitrust {

#define GEN_PASS_DECL_EMITRUSTCANONICALROUNDTRIP
#include "EmitRust/Conversion/Passes.h.inc"

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CONVERSION_CANONICALROUNDTRIP_H
