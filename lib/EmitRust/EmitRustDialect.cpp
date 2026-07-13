//===- EmitRustDialect.cpp - EmitRust dialect implementation --------------===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements the EmitRust dialect: registration of the dialect's operations,
/// types, and attributes.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/EmitRustDialect.h"

#include "EmitRust/EmitRustAttributes.h"
#include "EmitRust/EmitRustOps.h"
#include "EmitRust/EmitRustTypes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"

using namespace mlir;
using namespace mlir::emitrust;

#include "EmitRust/EmitRustOpsDialect.cpp.inc"

/// Registers the EmitRust operations, types, and attributes with the dialect.
void EmitRustDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "EmitRust/EmitRustOps.cpp.inc"
      >();
  registerTypes();
  registerAttributes();
}
