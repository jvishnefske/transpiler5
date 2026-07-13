//===- EmitRustAttributes.cpp - EmitRust attribute implementations --------===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements the EmitRust dialect attributes: the generated definitions of
/// the opaque attribute, the comparison predicate enum, and the enum
/// attribute wrapper, along with the generated attribute parser/printer.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/EmitRustAttributes.h"

#include "EmitRust/EmitRustDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using namespace mlir::emitrust;

#include "EmitRust/EmitRustEnums.cpp.inc"

#define GET_ATTRDEF_CLASSES
#include "EmitRust/EmitRustAttributes.cpp.inc"

/// Registers the EmitRust attributes with the dialect.
void EmitRustDialect::registerAttributes() {
  addAttributes<
#define GET_ATTRDEF_LIST
#include "EmitRust/EmitRustAttributes.cpp.inc"
      >();
}
