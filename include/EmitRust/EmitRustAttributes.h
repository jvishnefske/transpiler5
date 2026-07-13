//===- EmitRustAttributes.h - EmitRust attribute declarations ---*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares the EmitRust dialect attributes: the opaque expression attribute
/// and the comparison predicate enum attribute.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_EMITRUSTATTRIBUTES_H
#define EMITRUST_EMITRUSTATTRIBUTES_H

#include "mlir/IR/Attributes.h"
#include "mlir/IR/BuiltinAttributes.h"

#include "EmitRust/EmitRustEnums.h.inc"

#define GET_ATTRDEF_CLASSES
#include "EmitRust/EmitRustAttributes.h.inc"

#endif // EMITRUST_EMITRUSTATTRIBUTES_H
