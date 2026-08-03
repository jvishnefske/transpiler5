//===- EmitRustAttributes.td - EmitRust attributes ---------*- tablegen -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
// Defines the MLIR EmitRust attributes: the opaque expression attribute and
// the comparison predicate enum attribute.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_EMITRUSTATTRIBUTES
#define EMITRUST_EMITRUSTATTRIBUTES

include "mlir/IR/AttrTypeBase.td"
include "mlir/IR/BuiltinAttributeInterfaces.td"
include "mlir/IR/EnumAttr.td"
include "EmitRust/EmitRustBase.td"

//===----------------------------------------------------------------------===//
// EmitRust attribute definitions
//===----------------------------------------------------------------------===//

// Base class for EmitRust dialect attributes.
class EmitRust_Attr<string name, string attrMnemonic, list<Trait> traits = []>
    : AttrDef<EmitRust_Dialect, name, traits> {
  let mnemonic = attrMnemonic;
}

def EmitRust_OpaqueAttr : EmitRust_Attr<"Opaque", "opaque"> {
  let summary = "An opaque attribute";

  let description = [{
    An opaque attribute of which the value gets emitted as is, e.g. as a
    verbatim Rust constant expression.

    Example:

    ```mlir
    #emitrust.opaque<"String::new()">
    #emitrust.opaque<"i64::MAX">
    ```
  }];

  let parameters = (ins StringRefParameter<"the opaque value">:$value);
  let assemblyFormat = "`<` $value `>`";
}

// Attribute constraint accepting either an EmitRust opaque attribute or any
// typed attribute (mirrors EmitC_OpaqueOrTypedAttr).
def EmitRust_OpaqueOrTypedAttr
    : AnyAttrOf<[EmitRust_OpaqueAttr, TypedAttrInterface]>;

// Attribute constraint for global initializers: everything
// EmitRust_OpaqueOrTypedAttr admits, plus an ArrayAttr of element
// initializers for aggregate (array or struct) globals. The GlobalOp
// verifier checks that an ArrayAttr initializer structurally matches the
// global's value type (element count and per-element types).
def EmitRust_InitAttr
    : AnyAttrOf<[EmitRust_OpaqueAttr, TypedAttrInterface, ArrayAttr]>;

//===----------------------------------------------------------------------===//
// Comparison predicate enum
//===----------------------------------------------------------------------===//

// The comparison predicates supported by `emitrust.cmp`:
//
// - equal to                 (mnemonic: `eq`; integer value: `0`)
// - not equal to             (mnemonic: `ne`; integer value: `1`)
// - less than                (mnemonic: `lt`; integer value: `2`)
// - less than or equal to    (mnemonic: `le`; integer value: `3`)
// - greater than             (mnemonic: `gt`; integer value: `4`)
// - greater than or equal to (mnemonic: `ge`; integer value: `5`)
def EmitRust_CmpPredicate : I64EnumAttr<
    "CmpPredicate", "comparison predicate for emitrust.cmp",
    [
      I64EnumAttrCase<"eq", 0>,
      I64EnumAttrCase<"ne", 1>,
      I64EnumAttrCase<"lt", 2>,
      I64EnumAttrCase<"le", 3>,
      I64EnumAttrCase<"gt", 4>,
      I64EnumAttrCase<"ge", 5>,
    ]> {
  let cppNamespace = "::mlir::emitrust";
  let genSpecializedAttr = 0;
}

// Dialect attribute wrapping the CmpPredicate enum. Generates the C++ class
// `::mlir::emitrust::CmpPredicateAttr`.
def EmitRust_CmpPredicateAttr
    : EnumAttr<EmitRust_Dialect, EmitRust_CmpPredicate, "cmp_predicate">;

// The runtime flavor of an `emitrust.actor_runtime` anchor (FR-62 slice 5b):
// - threaded (mnemonic: `threaded`; integer value: `0`): the actor's state
//   moves to a spawned std::thread behind an mpsc mailbox; every call is a
//   message with a per-call typed reply channel.
// - async (mnemonic: `async`; integer value: `1`): the tokio task flavor
//   (E4); the attribute space is defined now, the emission lands in a later
//   slice.
// `same-thread` is deliberately NOT a case: the same-thread shape needs no
// runtime synthesis, so it has no anchor op at all.
def EmitRust_ActorMode : I64EnumAttr<
    "ActorMode", "runtime flavor of an emitrust.actor_runtime anchor",
    [
      I64EnumAttrCase<"threaded", 0>,
      I64EnumAttrCase<"async", 1>,
    ]> {
  let cppNamespace = "::mlir::emitrust";
  let genSpecializedAttr = 0;
}

// Dialect attribute wrapping the ActorMode enum. Generates the C++ class
// `::mlir::emitrust::ActorModeAttr`.
def EmitRust_ActorModeAttr
    : EnumAttr<EmitRust_Dialect, EmitRust_ActorMode, "actor_mode">;

#endif // EMITRUST_EMITRUSTATTRIBUTES
