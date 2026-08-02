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

//===----------------------------------------------------------------------===//
// FR-52 -- external requirements
//===----------------------------------------------------------------------===//

/// FR-52: name of the discardable `func.func`/`emitrust.func` unit attribute
/// the C importer attaches to a body-less external function that no
/// translation unit defines and that the project therefore REQUIRES from its
/// environment rather than provides.
///
/// The attribute only marks the fact; `emitrust-lower-external-requirements`
/// turns the marked declarations into one `emitrust.trait_def` and erases
/// them. A module reaching the Rust emitter with this attribute still set is
/// a bug (the emitter cannot render a body-less function), which is exactly
/// the pre-FR-52 behavior for an unresolved external.
inline constexpr llvm::StringLiteral kExternalRequirementAttrName =
    "emitrust.external_requirement";

/// FR-57a: name of the discardable unit attribute the deferred-externals
/// import mode attaches to a declaration-only global or function whose
/// DEFINITION lives in another translation unit's shard. The marked
/// `emitrust.global` has no initializer and the marked function no body;
/// both are link-time obligations, not storage or code. A module still
/// carrying this attribute CANNOT be emitted as Rust — the FR-58 link/merge
/// step must resolve every marked declaration against its defining
/// translation unit first, and the Rust emitter enforces that with a
/// located error rather than silently emitting a crate that reads a symbol
/// nobody defines.
inline constexpr llvm::StringLiteral kExternDeclAttrName =
    "emitrust.extern_decl";

/// FR-52: name of the discardable `emitrust.func` string attribute marking a
/// function that is GENERIC over the external-requirement trait. Its value is
/// the trait's name, so the emitter needs no module-level channel to render
/// the bound: the function prints as `fn f<E: <value>>(...)`.
///
/// Set on exactly the transitive closure of callers of a requirement, so a
/// function that needs nothing keeps the signature it always had.
inline constexpr llvm::StringLiteral kExternalsGenericAttrName =
    "emitrust.externals_generic";

/// FR-52: the name of the emitted trait. A project defining an item with this
/// name is a located error rather than a silent clash — see
/// `emitrust-lower-external-requirements`.
inline constexpr llvm::StringLiteral kExternalsTraitName = "Externals";

/// FR-52: the type-parameter name a requirement-generic function is
/// parameterized by (`fn f<E: Externals>`). A single letter keeps the
/// emitted signatures readable; the same clash check that guards the trait
/// name guards this one, because a Rust type parameter SHADOWS a same-named
/// type inside the generic item.
inline constexpr llvm::StringLiteral kExternalsTypeParam = "E";

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_EMITRUSTDIALECT_H
