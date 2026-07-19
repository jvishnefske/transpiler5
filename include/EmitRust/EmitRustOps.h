//===- EmitRustOps.h - EmitRust operation declarations ----------*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Declares the EmitRust dialect operations. Including this header pulls in
/// the dialect, type, and attribute declarations as well as the interface
/// headers the operations implement.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_EMITRUSTOPS_H
#define EMITRUST_EMITRUSTOPS_H

#include "EmitRust/EmitRustAttributes.h"
#include "EmitRust/EmitRustDialect.h"
#include "EmitRust/EmitRustTypes.h"
#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/CallInterfaces.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Interfaces/InferIntRangeInterface.h"
#include "mlir/Interfaces/InferTypeOpInterface.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#define GET_OP_CLASSES
#include "EmitRust/EmitRustOps.h.inc"

#endif // EMITRUST_EMITRUSTOPS_H
