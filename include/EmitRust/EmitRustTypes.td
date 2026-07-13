//===- EmitRustTypes.td - EmitRust types -------------------*- tablegen -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
// Defines the MLIR EmitRust types: opaque Rust type strings, shared /
// mutable references, assignable lvalues, one-dimensional arrays, and named
// struct references.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_EMITRUSTTYPES
#define EMITRUST_EMITRUSTTYPES

include "mlir/IR/AttrTypeBase.td"
include "EmitRust/EmitRustBase.td"

//===----------------------------------------------------------------------===//
// EmitRust type definitions
//===----------------------------------------------------------------------===//

// Base class for EmitRust dialect types.
class EmitRust_Type<string name, string typeMnemonic, list<Trait> traits = []>
    : TypeDef<EmitRust_Dialect, name, traits> {
  let mnemonic = typeMnemonic;
}

def EmitRust_OpaqueType : EmitRust_Type<"Opaque", "opaque"> {
  let summary = "EmitRust opaque type";

  let description = [{
    An opaque Rust type of which the value gets emitted as is.

    Example:

    ```mlir
    !emitrust.opaque<"String">
    !emitrust.opaque<"Vec<i32>">
    ```
  }];

  let parameters = (ins StringRefParameter<"the opaque value">:$value);
  let assemblyFormat = "`<` $value `>`";
  let genVerifyDecl = 1;
}

def EmitRust_RefType : EmitRust_Type<"Ref", "ref"> {
  let summary = "EmitRust shared reference type";

  let description = [{
    A shared (immutable) Rust reference type, emitted as `&T`.

    Example:

    ```mlir
    // Reference emitted as `&i32`
    !emitrust.ref<i32>
    // Reference emitted as `&String`
    !emitrust.ref<!emitrust.opaque<"String">>
    ```
  }];

  let parameters = (ins "Type":$pointee);
  let builders = [
    TypeBuilderWithInferredContext<(ins "Type":$pointee), [{
      return $_get(pointee.getContext(), pointee);
    }]>
  ];
  let assemblyFormat = "`<` qualified($pointee) `>`";
}

def EmitRust_MutRefType : EmitRust_Type<"MutRef", "mut_ref"> {
  let summary = "EmitRust mutable reference type";

  let description = [{
    A mutable Rust reference type, emitted as `&mut T`.

    Example:

    ```mlir
    // Reference emitted as `&mut i32`
    !emitrust.mut_ref<i32>
    // Reference emitted as `&mut String`
    !emitrust.mut_ref<!emitrust.opaque<"String">>
    ```
  }];

  let parameters = (ins "Type":$pointee);
  let builders = [
    TypeBuilderWithInferredContext<(ins "Type":$pointee), [{
      return $_get(pointee.getContext(), pointee);
    }]>
  ];
  let assemblyFormat = "`<` qualified($pointee) `>`";
}

def EmitRust_LValueType : EmitRust_Type<"LValue", "lvalue"> {
  let summary = "EmitRust lvalue type";

  let description = [{
    An assignable place. A value of this type denotes a memory location that
    can be read with `emitrust.load`, written with `emitrust.assign`, and
    borrowed with `emitrust.addr_of`. Places are produced by
    `emitrust.variable` and refined by `emitrust.member`,
    `emitrust.subscript`, and `emitrust.deref`.

    An lvalue type must never appear as a function parameter type, a
    function result type, or an `emitrust.func`/`emitrust.return` operand
    type (enforced by the `emitrust.func` verifier), and must not be nested
    inside another lvalue. It is never rendered as a Rust type; the emitter
    always uses the wrapped value type.

    Example:

    ```mlir
    !emitrust.lvalue<i32>
    !emitrust.lvalue<!emitrust.array<4xi32>>
    ```
  }];

  let parameters = (ins "Type":$value_type);
  let builders = [
    TypeBuilderWithInferredContext<(ins "Type":$value_type), [{
      return $_get(value_type.getContext(), value_type);
    }]>
  ];
  let assemblyFormat = "`<` qualified($value_type) `>`";
  let genVerifyDecl = 1;
}

def EmitRust_ArrayType : EmitRust_Type<"Array", "array"> {
  let summary = "EmitRust array type";

  let description = [{
    A one-dimensional array type with a compile-time size of at least one
    element. The element type must be a scalar (integer, index, or float) or
    an `!emitrust.struct` type; nested arrays, lvalues, and references are
    rejected. The type is emitted as the Rust array type `[T; N]`.

    Example:

    ```mlir
    // Array emitted as `[i32; 4]`
    !emitrust.array<4xi32>
    // Array emitted as `[Point; 2]`
    !emitrust.array<2x!emitrust.struct<"Point">>
    ```
  }];

  let parameters = (ins "uint64_t":$size, "Type":$element_type);

  let extraClassDeclaration = [{
    /// Returns whether `type` may be used as an array element type: a
    /// scalar (integer, index, f32, f64) or an EmitRust struct type.
    static bool isValidElementType(Type type);
  }];

  let genVerifyDecl = 1;
  let hasCustomAssemblyFormat = 1;
}

def EmitRust_StructType : EmitRust_Type<"Struct", "struct"> {
  let summary = "EmitRust struct type";

  let description = [{
    A named reference to a module-level `emitrust.struct_def`. The type is
    emitted as the bare struct name, e.g. `Point`. The name must be
    non-empty; no cross-checking against the referenced symbol is performed.

    Example:

    ```mlir
    // Struct type emitted as `Point`
    !emitrust.struct<"Point">
    ```
  }];

  let parameters = (ins StringRefParameter<"struct name">:$name);
  let assemblyFormat = "`<` $name `>`";
  let genVerifyDecl = 1;
}

#endif // EMITRUST_EMITRUSTTYPES
