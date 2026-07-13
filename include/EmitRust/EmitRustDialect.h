//===- EmitRustDialect.h - EmitRust dialect declaration ---------*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares the EmitRust dialect class. The dialect registers the EmitRust
/// operations, types, and attributes and provides the default type and
/// attribute printer/parser hooks.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_EMITRUSTDIALECT_H
#define EMITRUST_EMITRUSTDIALECT_H

#include "mlir/IR/Dialect.h"

#include "EmitRust/EmitRustOpsDialect.h.inc"

#endif // EMITRUST_EMITRUSTDIALECT_H
