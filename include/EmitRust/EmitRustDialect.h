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

/// FR-61e: name of the discardable `func.func`/`emitrust.func` ArrayAttr of
/// StringAttr carrying one slot per SIGNATURE INPUT with the parameter's
/// final Rust base spelling (importer-side `mangleMemberName`). A slot is
/// the empty string when the C parameter is unnamed, when the parameter's
/// value was copied into a named shadow variable (two bindings must never
/// share a spelling), when the input is a method receiver (`self` is
/// hard-wired), or when it is the synthesized second (cursor) input of a
/// string-cursor parameter. The Rust emitter binds each named argument
/// through the same per-function uniquifier as named locals.
inline constexpr llvm::StringLiteral kParamNamesAttrName =
    "emitrust.param_names";

/// W2.2: name of the discardable `func.func`/`emitrust.func` unit attribute
/// marking a method-of-tagged function that takes NO receiver (a
/// static/associated function, e.g. a C++ `static` member function).
/// `emitrust.impl`'s verifier skips the receiver-shape check on a so-marked
/// function, and the Rust emitter renders its full parameter list instead of
/// consuming argument 0 as `self`.
inline constexpr llvm::StringLiteral kStaticMethodAttrName =
    "emitrust.static_method";

/// FR-62 F2: name of the discardable `emitrust.struct_def` unit attribute
/// marking an exported owner struct whose FIELDS stay private. In library
/// (export) mode the emitter renders every struct's parts `pub` so the
/// exported type is usable; an owner-handle struct is the exception — its
/// state must only be constructed through the synthesized `new()` and
/// mutated through the exported methods, so its fields render with no
/// visibility prefix. Without `RustEmitOptions::exportItems` the attribute
/// is a no-op (binary-crate fields carry no `pub` to begin with).
inline constexpr llvm::StringLiteral kPrivateFieldsAttrName =
    "emitrust.private_fields";

/// W2.17: name of the discardable `emitrust.struct_def` unit attribute
/// marking a struct whose C++ class declared a destructor, i.e. a struct
/// the emitter renders with an `impl Drop`. It carries two consequences the
/// emitter cannot re-derive from the impl alone (the struct is rendered
/// before any impl is seen, and the liveness analyses run per function):
///
/// * `Copy` drops out of the derive list. Measured rustc E0184: "the trait
///   `Copy` cannot be implemented for this type; the type has a
///   destructor". `Clone`/`Default` stay -- both compile alongside `Drop`.
/// * Every binding of the struct is treated as READ on every path, so its
///   synthesized initializer can never be deferred and no store to it is
///   ever dead. An UNINITIALIZED Rust binding (`let x: T;`) is NEVER
///   dropped, so eliding the initializer would silently delete the
///   destructor's side effects -- a compile-clean miscompile (measured).
inline constexpr llvm::StringLiteral kHasDropAttrName = "emitrust.has_drop";

/// W2.23: name of the discardable `emitrust.struct_def` unit attribute the C
/// importer attaches to a class with an ADMITTED user copy constructor (the
/// user-provided `T(const T&)` shape). Its single consumer is the emitter's
/// derive list: `Copy` drops out, exactly the `has_drop` mechanism above
/// with a different trigger. This is load-bearing precisely for the
/// copy-ctor-WITHOUT-destructor class -- the only kind admitted BY VALUE --
/// where a bitwise Rust `Copy` at a by-value pass would silently substitute
/// for the user's constructor: 0 copies observed where C++ mandates 1, a
/// compile-clean miscompile (measured in the W2.23 spike). A copy+dtor
/// class carries both attributes; either alone already suppresses `Copy`.
inline constexpr llvm::StringLiteral kHasCopyCtorAttrName =
    "emitrust.has_copy_ctor";

/// W2.17: name of the discardable `func.func`/`emitrust.func` unit attribute
/// the C importer attaches to an imported C++ destructor body (the W2.2
/// `emitrust.static_method` precedent). `convert-func-to-emitrust` consumes
/// it: instead of joining the class's INHERENT `emitrust.impl`, the function
/// is routed into a second `emitrust.impl` for the same struct carrying
/// `trait_name = "Drop"`, and is RENAMED to the symbol `drop` (rustc E0407
/// otherwise). The rename is safe because `emitrust.impl` is a SymbolTable
/// and nothing in the subset ever calls a destructor.
inline constexpr llvm::StringLiteral kDropImplAttrName = "emitrust.drop_impl";

//===----------------------------------------------------------------------===//
// FR-52 -- external requirements
//===----------------------------------------------------------------------===//

/// FR-52: name of the discardable `func.func`/`emitrust.func` unit attribute
/// the C importer attaches to a body-less external function that no
/// translation unit defines and that the project therefore REQUIRES from its
/// environment rather than provides. FR-70 attaches the same marker to a
/// declaration-only `emitrust.global` whose undefined external STORAGE
/// qualifies as a requirement (scalar, address never taken, every use a
/// whole-value load or store).
///
/// The attribute only marks the fact; `emitrust-lower-external-requirements`
/// turns the marked declarations into one `emitrust.trait_def` (a global
/// contributing a getter/setter pair) and erases them. A module reaching the
/// Rust emitter with this attribute still set is a bug: a marked function
/// dies naturally (the emitter cannot render a body-less function), and a
/// marked GLOBAL — which would otherwise silently render as defaulted
/// storage — is refused explicitly by the emitter.
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

/// FR-78: name of the discardable unit attribute marking a struct_def that
/// models a differing-aggregate-arm C union as OPAQUE STORAGE — a single
/// sizeof-sized byte-blob field. The type and every whole-value use are
/// admitted (the containment win: records naming the union import), but
/// the blob carries no arm-typed view, so the importer must reject every
/// access through any union arm at its own site. Nothing structural backs
/// that promise up — `emitrust.member` field names are not cross-checked
/// against the struct_def, so a leaked arm access verifies, translates,
/// and dies only as rustc E0609 (a whole-crate loss with no source
/// location). The Rust emitter therefore refuses ANY member selection on a
/// marked struct type, marker-contract style (see FR-52/FR-57a above).
inline constexpr llvm::StringLiteral kOpaqueUnionAttrName =
    "emitrust.opaque_union";

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
