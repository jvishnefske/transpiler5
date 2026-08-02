//===- EmitRustTypes.td - EmitRust types -------------------*- tablegen -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
// Defines the MLIR EmitRust types: opaque Rust type strings, shared /
// mutable references, assignable lvalues, fixed-size (possibly nested)
// arrays, dynamically sized slices, nullable function pointers, and named
// struct and enum references.
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
    An array type with a compile-time size of at least one element. The
    element type must be a scalar (integer, index, or float), an
    `!emitrust.struct` type, an `!emitrust.enum` type (a C array of
    enums, emitted as `[Color; N]`), an `!emitrust.fn_ptr` type (a C
    function-pointer table, emitted as `[Option<fn(...)>; N]`), or a
    nested `!emitrust.array` type (a C multi-dimensional array is an
    array of arrays); lvalues, slices, and references are rejected. The
    type is emitted as the Rust array type `[T; N]`, nesting as
    `[[T; N]; M]`.

    Example:

    ```mlir
    // Array emitted as `[i32; 4]`
    !emitrust.array<4xi32>
    // Array emitted as `[Point; 2]`
    !emitrust.array<2x!emitrust.struct<"Point">>
    // Two-dimensional array emitted as `[[i8; 4]; 2]`
    !emitrust.array<2x!emitrust.array<4xi8>>
    ```
  }];

  let parameters = (ins "uint64_t":$size, "Type":$element_type);

  let extraClassDeclaration = [{
    /// Returns whether `type` may be used as an array element type: a
    /// scalar (integer, index, f32, f64), an EmitRust struct type, a
    /// function pointer, or a nested EmitRust array type.
    static bool isValidElementType(Type type);
  }];

  let genVerifyDecl = 1;
  let hasCustomAssemblyFormat = 1;
}

def EmitRust_SliceType : EmitRust_Type<"Slice", "slice"> {
  let summary = "EmitRust slice type";

  let description = [{
    A dynamically sized view of a run of elements, emitted as the Rust slice
    type `[T]`. The element type must be a scalar (integer, index, or
    float) or an `!emitrust.struct` type; unlike array elements, nested
    arrays are rejected (slices only ever view runs of scalar or struct
    elements).

    Because a slice is unsized, the type is only valid behind a reference
    (`!emitrust.ref`/`!emitrust.mut_ref`, rendering `&[T]`/`&mut [T]`) or as
    the value type of an `!emitrust.lvalue` (the place designated by
    dereferencing such a reference). It must not appear as the value type of
    an `emitrust.variable` or `emitrust.global`, as a struct field type, or
    as an array element type.

    Example:

    ```mlir
    // Parameter type emitted as `&mut [i32]`
    !emitrust.mut_ref<!emitrust.slice<i32>>
    ```
  }];

  let parameters = (ins "Type":$element_type);
  let builders = [
    TypeBuilderWithInferredContext<(ins "Type":$element_type), [{
      return $_get(element_type.getContext(), element_type);
    }]>
  ];

  let extraClassDeclaration = [{
    /// Returns whether `type` may be used as a slice element type: a
    /// scalar (integer, index, f32, f64) or an EmitRust struct type.
    /// Nested arrays are rejected.
    static bool isValidElementType(Type type);
  }];

  let assemblyFormat = "`<` qualified($element_type) `>`";
  let genVerifyDecl = 1;
}

def EmitRust_CellSliceType : EmitRust_Type<"CellSlice", "cell_slice"> {
  let summary = "EmitRust cell-slice type";

  let description = [{
    A dynamically sized run of `Cell` elements, emitted as the Rust slice
    type `[std::cell::Cell<T>]`. The element type must be a scalar
    (integer, index, or float). The type models the interprocedural
    pointer-parameter class whose bases are all mutable globals (CTS-P10):
    a callee may mutate elements through the parameter while other code
    reads the same globals directly, and both sides observe every write
    because `emitrust.cell_get`/`emitrust.cell_set` and
    `emitrust.global_load`/`emitrust.global_store` hit the same
    thread-local `Cell` storage.

    Because a cell-slice is unsized, the type is only valid behind the
    shared `!emitrust.ref` reference (rendering `&[std::cell::Cell<T>]`).
    Cells provide interior mutability, so no `!emitrust.mut_ref` form
    exists; the shared reference is freely duplicable, which is what lets
    permuted recursive forwarding pass the same parameters along without
    any reborrow discipline. It must not appear as the value type of an
    `emitrust.variable` or `emitrust.global`, as a struct field type, or
    as an array or slice element type.

    Example:

    ```mlir
    // Parameter type emitted as `&[std::cell::Cell<i32>]`
    !emitrust.ref<!emitrust.cell_slice<i32>>
    ```
  }];

  let parameters = (ins "Type":$element_type);
  let builders = [
    TypeBuilderWithInferredContext<(ins "Type":$element_type), [{
      return $_get(element_type.getContext(), element_type);
    }]>
  ];

  let extraClassDeclaration = [{
    /// Returns whether `type` may be used as a cell-slice element type: a
    /// scalar (integer, index, f32, f64). Aggregates, references, lvalues,
    /// and dialect types are rejected.
    static bool isValidElementType(Type type);
  }];

  let assemblyFormat = "`<` qualified($element_type) `>`";
  let genVerifyDecl = 1;
}

def EmitRust_FnPtrType : EmitRust_Type<"FnPtr", "fn_ptr"> {
  let summary = "EmitRust nullable function pointer type";

  let description = [{
    A nullable function pointer, emitted as the Rust type
    `Option<fn(A, B) -> R>`. The C null pointer maps to `None` and a
    function reference to `Some(name)`, so the type is `Copy`, `PartialEq`,
    and `Default` (`None`), making it a legal struct field, global,
    variable, parameter, and result without any sentinel value.

    The assembly syntax always parenthesizes the parameter list; the
    `-> R` result clause is present if and only if the function returns a
    value (the void spelling omits it entirely):

    ```mlir
    !emitrust.fn_ptr<(i32, i32) -> i32>  // fn(i32, i32) -> i32
    !emitrust.fn_ptr<() -> i32>          // zero parameters
    !emitrust.fn_ptr<(i32)>              // void result
    !emitrust.fn_ptr<()>                 // zero parameters, void result
    ```

    Component (parameter and result) types are restricted to the
    emitter-supported scalars (i1, i8/i16/i32/i64, u8/u16/u32/u64, index,
    f32, f64), `!emitrust.struct`, `!emitrust.enum`, and nested
    `!emitrust.fn_ptr`; pointers, references, lvalues, and arrays are
    rejected.
  }];

  let parameters = (ins ArrayRefParameter<"Type">:$inputs,
                        ArrayRefParameter<"Type">:$results);

  let extraClassDeclaration = [{
    /// Returns whether `type` may be used as a fn_ptr parameter or result
    /// type: an emitter-supported scalar (i1, i8/i16/i32/i64 signless,
    /// u8/u16/u32/u64, index, f32, f64), an EmitRust struct or enum, or a
    /// nested fn_ptr. References, lvalues, arrays, and opaque types are
    /// rejected.
    static bool isValidComponentType(Type type);
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

def EmitRust_EnumType : EmitRust_Type<"Enum", "enum"> {
  let summary = "EmitRust enum type";

  let description = [{
    A named reference to a module-level `emitrust.enum_def`. The type is
    emitted as the bare enum name, e.g. `Color`. The name must be non-empty;
    no cross-checking against the referenced symbol is performed.

    Enum values may be used as variable/lvalue value types, function
    parameter and result types, `emitrust.struct_def` field types,
    `emitrust.constant` results (with an opaque attribute such as
    `Color::Red`), `emitrust.cmp` operands (with the `eq` and `ne`
    predicates only), `emitrust.cast` sources (rendering `.0 as i32` on
    the open-enum tuple-struct representation), and `emitrust.cast`
    results with a non-`i1` integer source (an integer-to-enum conversion,
    rendering the value-preserving tuple-struct constructor). They must
    not appear as arithmetic binary operands. A default-initialized
    variable of enum type renders `Name::default()`, and `emitrust.enum_raw`
    projects an enum lvalue to the lvalue of its raw storage integer.

    Example:

    ```mlir
    // Enum type emitted as `Color`
    !emitrust.enum<"Color">
    ```
  }];

  let parameters = (ins StringRefParameter<"enum name">:$name);
  let assemblyFormat = "`<` $name `>`";
  let genVerifyDecl = 1;
}

#endif // EMITRUST_EMITRUSTTYPES
