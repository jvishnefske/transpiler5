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

namespace mlir {
namespace emitrust {

/// Name of the discardable `func.func` attribute the C importer attaches to
/// a function that must become a `&mut self` method of an owner struct. Its
/// `StringAttr` value is the owner struct's name; `convert-func-to-emitrust`
/// materializes the function inside the matching `emitrust.impl` and strips
/// the attribute.
inline constexpr llvm::StringLiteral kMethodOfAttrName = "emitrust.method_of";

/// Name of the discardable `func.call` unit attribute the C importer
/// attaches to a call whose callee is a method-planned function. The call's
/// first operand is an `emitrust.addr_of mut` of the owner place;
/// `convert-func-to-emitrust` rewrites the call into an
/// `emitrust.method_call` on that place and erases the dead borrow.
inline constexpr llvm::StringLiteral kMethodCallAttrName =
    "emitrust.method_call";

/// W2.2: name of the discardable `func.func`/`emitrust.func` unit attribute
/// marking a method-of-tagged function that takes NO receiver (a
/// static/associated function, e.g. a C++ `static` member function).
/// `emitrust.impl`'s verifier skips the receiver-shape check on a so-marked
/// function, and the Rust emitter renders its full parameter list instead of
/// consuming argument 0 as `self`.
inline constexpr llvm::StringLiteral kStaticMethodAttrName =
    "emitrust.static_method";

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_EMITRUSTDIALECT_H
