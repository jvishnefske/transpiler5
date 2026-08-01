//===- LowerContainers.h - Container op lowering ----------------*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares the emitrust-lower-containers pass: lowers the backend-agnostic
/// container ops (`emitrust.collection`/`collection_push`/`collection_at`) to
/// the production array backend — a fixed `[T;CAP]` pool (`emitrust.variable`)
/// plus a rank-0 `memref` i64 free cursor, the FR-39 node-pool shape. A
/// growable `Vec<T>` backend is deferred to the heap RFC (design.md W4.5).
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CONVERSION_LOWERCONTAINERS_H
#define EMITRUST_CONVERSION_LOWERCONTAINERS_H

#include <memory>

namespace mlir {
class Pass;

namespace emitrust {

#define GEN_PASS_DECL_EMITRUSTLOWERCONTAINERS
#include "EmitRust/Conversion/Passes.h.inc"

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CONVERSION_LOWERCONTAINERS_H
