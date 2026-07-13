//===- EmitRustTypes.h - EmitRust type declarations -------------*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares the EmitRust dialect types: the opaque Rust type string type and
/// the shared/mutable reference types.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_EMITRUSTTYPES_H
#define EMITRUST_EMITRUSTTYPES_H

#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Types.h"

#define GET_TYPEDEF_CLASSES
#include "EmitRust/EmitRustOpsTypes.h.inc"

#endif // EMITRUST_EMITRUSTTYPES_H
