//===- EmitRustOps.td - EmitRust operations ----------------*- tablegen -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
// Defines the MLIR EmitRust operations. This is the top-level TableGen file
// of the dialect; it includes the base, type, and attribute definitions.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_EMITRUSTOPS
#define EMITRUST_EMITRUSTOPS

include "EmitRust/EmitRustAttributes.td"
include "EmitRust/EmitRustTypes.td"

include "mlir/IR/SymbolInterfaces.td"
include "mlir/Interfaces/CallInterfaces.td"
include "mlir/Interfaces/ControlFlowInterfaces.td"
include "mlir/Interfaces/FunctionInterfaces.td"
include "mlir/Interfaces/InferIntRangeInterface.td"
include "mlir/Interfaces/InferTypeOpInterface.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

//===----------------------------------------------------------------------===//
// Common type constraints
//===----------------------------------------------------------------------===//

// Types usable in arithmetic and comparison operations: any integer
// (signless, signed, or unsigned), index, f32, f64, or an opaque Rust type.
def EmitRust_ArithmeticType
    : AnyTypeOf<[AnyInteger, Index, F32, F64, EmitRust_OpaqueType]>;

// Types usable as loop bounds: any integer or index.
def EmitRust_IntegerOrIndexType : AnyTypeOf<[AnyInteger, Index]>;

// Types usable as comparison operands: any arithmetic type, an EmitRust
// enum type, or an EmitRust fn_ptr type. Enum and fn_ptr operands are
// restricted to the eq/ne predicates by the CmpOp verifier.
def EmitRust_CmpOperandType
    : AnyTypeOf<[EmitRust_ArithmeticType, EmitRust_EnumType,
                 EmitRust_FnPtrType]>;

// Types usable as select values: any arithmetic type (including bool) or
// an EmitRust struct, enum, array, or fn_ptr type (no lvalues or
// references).
def EmitRust_SelectValueType
    : AnyTypeOf<[EmitRust_ArithmeticType, EmitRust_StructType,
                 EmitRust_EnumType, EmitRust_ArrayType,
                 EmitRust_FnPtrType]>;

//===----------------------------------------------------------------------===//
// UseOp
//===----------------------------------------------------------------------===//

def EmitRust_UseOp : EmitRust_Op<"use", [HasParent<"::mlir::ModuleOp">]> {
  let summary = "Use declaration operation";
  let description = [{
    The `emitrust.use` operation emits a Rust `use` declaration bringing the
    given path into scope. It may only appear at module level.

    Example:

    ```mlir
    emitrust.use "std::vec::Vec"
    ```
    ```rust
    // Code emitted for the operation above.
    use std::vec::Vec;
    ```
  }];

  let arguments = (ins StrAttr:$path);
  let assemblyFormat = "$path attr-dict";
}

//===----------------------------------------------------------------------===//
// VerbatimOp
//===----------------------------------------------------------------------===//

def EmitRust_VerbatimOp : EmitRust_Op<"verbatim"> {
  let summary = "Verbatim operation";
  let description = [{
    The `emitrust.verbatim` operation produces no results and its value is
    emitted as is, followed by a line break, during translation.

    Note: Use with caution. This operation can have arbitrary effects on the
    semantics of the emitted code. Use semantically more meaningful
    operations whenever possible.

    Example:

    ```mlir
    emitrust.verbatim "#![allow(dead_code)]"
    ```
  }];

  let arguments = (ins StrAttr:$value);
  let assemblyFormat = "$value attr-dict";
}

//===----------------------------------------------------------------------===//
// FuncOp
//===----------------------------------------------------------------------===//

def EmitRust_FuncOp : EmitRust_Op<"func", [
  AutomaticAllocationScope, FunctionOpInterface, IsolatedFromAbove, Symbol
]> {
  let summary = "An operation with a name containing a single `SSACFG` region";
  let description = [{
    The `emitrust.func` operation represents a Rust function item. Operations
    within the function cannot implicitly capture values defined outside of
    the function, i.e. functions are `IsolatedFromAbove`. All external
    references must use function arguments or attributes that establish a
    symbolic connection. The function may have at most one result.

    Example:

    ```mlir
    emitrust.func @add(%a: i32, %b: i32) -> i32 {
      %sum = emitrust.add %a, %b : i32
      emitrust.return %sum : i32
    }
    ```
    ```rust
    // Code emitted for the operation above.
    fn add(v1: i32, v2: i32) -> i32 {
        let v3 = v1 + v2;
        return v3;
    }
    ```
  }];

  let arguments = (ins SymbolNameAttr:$sym_name,
                       TypeAttrOf<FunctionType>:$function_type,
                       OptionalAttr<DictArrayAttr>:$arg_attrs,
                       OptionalAttr<DictArrayAttr>:$res_attrs);
  let regions = (region AnyRegion:$body);

  let builders = [OpBuilder<(ins
    "::llvm::StringRef":$name, "::mlir::FunctionType":$type,
    CArg<"::llvm::ArrayRef<::mlir::NamedAttribute>", "{}">:$attrs,
    CArg<"::llvm::ArrayRef<::mlir::DictionaryAttr>", "{}">:$argAttrs)
  >];

  let extraClassDeclaration = [{
    //===------------------------------------------------------------------===//
    // FunctionOpInterface Methods
    //===------------------------------------------------------------------===//

    /// Returns the region on the current operation that is callable. This may
    /// return null in the case of an external callable object, e.g. an
    /// external function.
    ::mlir::Region *getCallableRegion() {
      return isExternal() ? nullptr : &getBody();
    }

    /// Returns the argument types of this function.
    ::llvm::ArrayRef<::mlir::Type> getArgumentTypes() {
      return getFunctionType().getInputs();
    }

    /// Returns the result types of this function.
    ::llvm::ArrayRef<::mlir::Type> getResultTypes() {
      return getFunctionType().getResults();
    }
  }];

  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// ImplOp
//===----------------------------------------------------------------------===//

def EmitRust_ImplOp : EmitRust_Op<"impl", [
    HasParent<"::mlir::ModuleOp">, SymbolTable, NoRegionArguments,
    NoTerminator]> {
  let summary = "Inherent (or Drop trait) impl block for an owner struct";
  let description = [{
    The `emitrust.impl` operation renders a Rust inherent `impl` block for
    the named struct. Its single region holds only `emitrust.func`
    operations, each of which must take the receiver as its first argument:
    an `!emitrust.mut_ref` of the `!emitrust.struct` carrying the impl's
    struct name. Inside the impl, the emitter names the receiver argument
    `self` and renders the signature with `&mut self`, so the existing
    dereference place rendering yields `(*self).field...` naturally. The
    struct name is not cross-checked against an `emitrust.struct_def`
    (mirroring `emitrust.member`).

    Example:

    ```mlir
    emitrust.impl "Owner" {
      emitrust.func @get(%self: !emitrust.mut_ref<!emitrust.struct<"Owner">>,
                         %i: i64) -> i32 {
        ...
      }
    }
    ```
    ```rust
    // Code emitted for the operation above.
    impl Owner {
        fn get(&mut self, v0: i64) -> i32 {
            ...
        }
    }
    ```

    W2.17: the optional `trait_name` attribute turns the block into a TRAIT
    impl -- `impl <trait_name> for <struct_name>` -- and `Drop` is the only
    modeled trait. A struct may carry both an inherent impl and a `Drop`
    impl; every consumer that looks an impl up BY STRUCT NAME (method
    mutability, actor derivation) skips the trait-carrying ones, so a
    module with no destructor is byte-identical to before. The attribute is
    covered by `attr-dict`, so the assembly format is unchanged.

    ```mlir
    emitrust.impl "Owner" {
      emitrust.func @drop(%self: !emitrust.mut_ref<!emitrust.struct<"Owner">>) {
        ...
      }
    } {trait_name = "Drop"}
    ```
    ```rust
    impl Drop for Owner {
        fn drop(&mut self) {
            ...
        }
    }
    ```
  }];

  let arguments = (ins StrAttr:$struct_name, OptionalAttr<StrAttr>:$trait_name);
  let regions = (region SizedRegion<1>:$body);

  // The inherent-impl builder every pre-W2.17 caller uses, unchanged: the
  // optional trait name defaults to absent.
  let builders = [
    OpBuilder<(ins "::llvm::StringRef":$structName), [{
      build($_builder, $_state, $_builder.getStringAttr(structName),
            ::mlir::StringAttr());
    }]>,
    OpBuilder<(ins "::mlir::StringAttr":$structName), [{
      build($_builder, $_state, structName, ::mlir::StringAttr());
    }]>
  ];

  let assemblyFormat = "$struct_name $body attr-dict";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// MethodCallOp
//===----------------------------------------------------------------------===//

def EmitRust_MethodCallOp : EmitRust_Op<"method_call"> {
  let summary = "Method call on a struct or opaque place";
  let description = [{
    The `emitrust.method_call` operation calls the named method on the
    receiver place, which must be an lvalue wrapping an `!emitrust.struct`
    OR an `!emitrust.opaque` type (W2.3: the STL recognition surface —
    `Vec<T>`/`String` inherent methods like `push`/`len`/`is_empty`/`clear`/
    `push_str` — reuses this op unchanged rather than adding a parallel
    mechanism; the opaque case is intentionally NOT cross-checked against
    any real Rust method table, mirroring `emitrust.call_opaque`'s trust
    model).  It renders as `place.method(args)` — Rust's auto-ref takes the
    `&mut` borrow of the place and scopes it to the call expression, so no borrow
    is ever materialized as a binding. The operation produces at most one
    result and is emitted as a `let` binding when it produces one and as a
    bare statement otherwise. The method name is not cross-checked against
    the corresponding `emitrust.impl` (mirroring `emitrust.member`).

    Example:

    ```mlir
    %r = emitrust.method_call %owner["get"](%i)
        : (!emitrust.lvalue<!emitrust.struct<"Owner">>, i64) -> i32
    ```
    ```rust
    // Code emitted for the operation above.
    let v2: i32 = v0.get(v1);
    ```
  }];

  let arguments = (ins EmitRust_LValueType:$receiver, StrAttr:$method,
                       Variadic<AnyType>:$args);
  let results = (outs Variadic<AnyType>);

  let assemblyFormat = [{
    $receiver `[` $method `]` `(` $args `)` attr-dict
    `:` functional-type(operands, results)
  }];
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// ReturnOp
//===----------------------------------------------------------------------===//

def EmitRust_ReturnOp : EmitRust_Op<"return", [Pure, HasParent<"FuncOp">,
                                               ReturnLike, Terminator]> {
  let summary = "Function return operation";
  let description = [{
    The `emitrust.return` operation represents a return within a function.
    The operation takes zero or exactly one operand and produces no results.
    The operand number and type must match the signature of the function
    that contains the operation.

    Example:

    ```mlir
    emitrust.func @foo() -> i32 {
      %0 = emitrust.constant <42 : i32> : i32
      emitrust.return %0 : i32
    }
    ```
  }];

  let arguments = (ins Optional<AnyType>:$operand);

  let assemblyFormat = "attr-dict ($operand^ `:` type($operand))?";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// CallOpaqueOp
//===----------------------------------------------------------------------===//

def EmitRust_CallOpaqueOp : EmitRust_Op<"call_opaque"> {
  let summary = "Opaque call operation";
  let description = [{
    The `emitrust.call_opaque` operation represents a call to an arbitrary
    Rust callable identified by a non-empty string.

    The optional `args` attribute controls the rendered argument list. When
    present, arguments are taken from `args` in order: an `IntegerAttr` of
    `index` type selects the operand with that index, a `StringAttr` renders
    as a quoted, escaped Rust string literal, and any other typed attribute
    renders as a constant expression. This makes it possible to pass literal
    format strings to Rust macros, which require the format string to be a
    literal inside the call. When `args` is absent, all operands are
    rendered in order.

    Example:

    ```mlir
    %0 = emitrust.call_opaque "foo"(%arg0) : (i32) -> i32
    emitrust.call_opaque "println!"() : () -> ()
    emitrust.call_opaque "print!"(%v) {args = ["x={}\n", 0 : index]}
        : (i32) -> ()
    ```
    ```rust
    // Code emitted for the last operation above.
    print!("x={}\n", v0);
    ```
  }];

  let arguments = (ins
    Arg<StrAttr, "the Rust function to call">:$callee,
    OptionalAttr<ArrayAttr>:$args,
    Variadic<AnyType>:$operands
  );
  let results = (outs Variadic<AnyType>);

  let assemblyFormat = [{
    $callee `(` $operands `)` attr-dict `:` functional-type($operands, results)
  }];
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// CallIndirectOp
//===----------------------------------------------------------------------===//

def EmitRust_CallIndirectOp : EmitRust_Op<"call_indirect"> {
  let summary = "Indirect call through a function pointer";
  let description = [{
    The `emitrust.call_indirect` operation calls through an
    `!emitrust.fn_ptr` value. The argument types must equal the callee's
    parameter types and the result (at most one) must equal the callee's
    result type; both are enforced by the verifier.

    The operation is emitted as
    `callee.expect("null function pointer")(args)`: calling a null C
    function pointer is undefined behavior, which this rendering refines
    into a deterministic panic.

    Example:

    ```mlir
    %0 = emitrust.call_indirect %f(%a, %b)
        : (!emitrust.fn_ptr<(i32, i32) -> i32>, i32, i32) -> i32
    ```
    ```rust
    // Code emitted for the operation above.
    let v3: i32 = v0.expect("null function pointer")(v1, v2);
    ```
  }];

  let arguments = (ins EmitRust_FnPtrType:$callee, Variadic<AnyType>:$args);
  let results = (outs Variadic<AnyType>);

  let assemblyFormat = [{
    $callee `(` $args `)` attr-dict `:` functional-type(operands, results)
  }];
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// ConstantOp
//===----------------------------------------------------------------------===//

def EmitRust_ConstantOp : EmitRust_Op<"constant",
    [DeclareOpInterfaceMethods<InferIntRangeInterface,
                               ["inferResultRanges"]>]> {
  let summary = "Constant operation";
  let description = [{
    The `emitrust.constant` operation produces an SSA value equal to some
    constant specified by an attribute. The attribute is either a typed
    attribute or an EmitRust opaque attribute whose value is emitted as is.

    The operation implements `InferIntRangeInterface`: an integer-typed
    constant infers the exact singleton range of its value; an opaque or
    non-integer constant stays at the full range of its type.

    Example:

    ```mlir
    %0 = emitrust.constant <42 : i32> : i32
    %1 = emitrust.constant <#emitrust.opaque<"i64::MAX">> : i64
    ```
  }];

  let arguments = (ins EmitRust_OpaqueOrTypedAttr:$value);
  let results = (outs AnyType:$result);

  let assemblyFormat = "` ` `<` $value `>` attr-dict `:` type($result)";
}

//===----------------------------------------------------------------------===//
// LiteralOp
//===----------------------------------------------------------------------===//

def EmitRust_LiteralOp : EmitRust_Op<"literal",
    [Pure, DeclareOpInterfaceMethods<InferIntRangeInterface,
                                     ["inferResultRanges"]>]> {
  let summary = "Literal operation";
  let description = [{
    The `emitrust.literal` operation produces an SSA value whose textual
    representation is the given non-empty string, emitted verbatim as a Rust
    expression.

    The operation implements `InferIntRangeInterface` conservatively: the
    literal text is opaque, so an integer-typed result is pinned to the
    full range of its type.

    Example:

    ```mlir
    %l = emitrust.literal "x.len()" : index
    ```
  }];

  let arguments = (ins StrAttr:$value);
  let results = (outs AnyType:$result);

  let assemblyFormat = "$value attr-dict `:` type($result)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// StringRepeatOp
//===----------------------------------------------------------------------===//

def EmitRust_StringRepeatOp : EmitRust_Op<"string_repeat", [Pure]> {
  let summary = "Idiomatic `String::repeat` of a constant single-byte fill";
  let description = [{
    The `emitrust.string_repeat` operation constructs an owned `String` by
    repeating the single ASCII character `$fill` exactly `$count` times. It is
    the fused lowering of the C `char *a = malloc(N); for (i..) a[i]=C; a[N-1]=0`
    constant-fill string idiom (FR-64) and renders as
    `"<fill>".repeat((<count>) as usize)`.

    The `$fill` attribute is a one-character string whose byte is ASCII
    `0x01..0x7F` — a valid, non-NUL, single-byte UTF-8 scalar — so the produced
    `String`'s bytes equal the original C buffer's bytes exactly (the byte-diff
    soundness gate; a multi-byte or NUL fill is rejected at import). The
    `$count` operand is an `i64` fill count widened from the C size expression.

    The result type is always `!emitrust.opaque<"String">`.

    Example:

    ```mlir
    %s = emitrust.string_repeat "a", %n : i64 -> !emitrust.opaque<"String">
    ```
    ```rust
    // Code emitted for the operation above.
    let a: String = "a".repeat((n) as usize);
    ```
  }];

  let arguments = (ins I64:$count, StrAttr:$fill);
  let results = (outs EmitRust_OpaqueType:$result);

  let assemblyFormat =
      "$fill `,` $count attr-dict `:` type($count) `->` type($result)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// VecFillOp
//===----------------------------------------------------------------------===//

def EmitRust_VecFillOp : EmitRust_Op<"vec_fill", [Pure]> {
  let summary = "Idiomatic `vec![<zero>; n]` runtime-sized heap buffer";
  let description = [{
    The `emitrust.vec_fill` operation constructs an owned `Vec<T>` of `$count`
    zero-initialized elements. It is the lowering of the C runtime-sized heap
    buffer idiom `T *a = malloc(n * sizeof(T))` / `calloc(n, sizeof(T))` for a
    non-char scalar element type `T` used only through `a[i]` indexing and
    `free` (FR-65, the Vec arm of the {array, Vec, span, Option} representation
    match) and renders as `vec![<fill>; (<count>) as usize]`.

    The `$fill` attribute is the suffixed Rust zero literal for the element
    type (`"0i32"`, `"0u32"`, `"0.0f64"`, ...). Zero is the sound refinement of
    `malloc`'s indeterminate bytes — a defined program writes every element
    before reading it — and matches `calloc`'s pre-zeroing exactly. The
    `$count` operand is an `i64` element count widened from the C size
    expression; it widens to `usize` at the fill site.

    The result type is always `!emitrust.opaque<"Vec<T>">`.

    Example:

    ```mlir
    %v = emitrust.vec_fill "0i32", %n : i64 -> !emitrust.opaque<"Vec<i32>">
    ```
    ```rust
    // Code emitted for the operation above.
    let a: Vec<i32> = vec![0i32; (n) as usize];
    ```
  }];

  let arguments = (ins I64:$count, StrAttr:$fill);
  let results = (outs EmitRust_OpaqueType:$result);

  let assemblyFormat =
      "$fill `,` $count attr-dict `:` type($count) `->` type($result)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// LetOp
//===----------------------------------------------------------------------===//

def EmitRust_LetOp : EmitRust_Op<"let", [AllTypesMatch<["init", "result"]>]> {
  let summary = "Let binding operation";
  let description = [{
    The `emitrust.let` operation represents a Rust `let` binding initialized
    with the given value. Bindings are immutable by default; the optional
    `mut` marker makes the binding mutable, which is a prerequisite for it
    being the destination of an `emitrust.assign`.

    Example:

    ```mlir
    %v = emitrust.let %init : i32
    %w = emitrust.let mut %init : i32
    ```
    ```rust
    // Code emitted for the operations above.
    let v2 = v1;
    let mut v3 = v1;
    ```
  }];

  let arguments = (ins AnyType:$init, UnitAttr:$is_mut);
  let results = (outs AnyType:$result);

  let assemblyFormat = "(`mut` $is_mut^)? $init attr-dict `:` type($result)";
}

//===----------------------------------------------------------------------===//
// AssignOp
//===----------------------------------------------------------------------===//

def EmitRust_AssignOp : EmitRust_Op<"assign"> {
  let summary = "Assign operation";
  let description = [{
    The `emitrust.assign` operation stores an SSA value to a destination.
    Two kinds of destinations are supported, mirroring Rust's mutability
    discipline:

    - A mutable let binding: the destination must be the result of an
      `emitrust.let` operation carrying the `mut` marker, and the value must
      be of the same type as the destination.
    - An lvalue: the destination is any value of `!emitrust.lvalue` type
      (produced by `emitrust.variable`, `emitrust.member`,
      `emitrust.subscript`, or `emitrust.deref`), and the value must be of
      the lvalue's wrapped value type.

    Assignment to anything else is rejected by the verifier. The operation
    produces no results. The textual type is the type of the destination;
    for an lvalue destination the value operand resolves to the lvalue's
    wrapped value type.

    Example:

    ```mlir
    %v = emitrust.let mut %init : i32
    emitrust.assign %v = %next : i32
    %p = emitrust.variable : !emitrust.lvalue<i32>
    emitrust.assign %p = %next : !emitrust.lvalue<i32>
    ```
    ```rust
    // Code emitted for the operations above.
    let mut v2 = v1;
    v2 = v3;
    let mut v4: i32 = 0;
    v4 = v3;
    ```
  }];

  let arguments = (ins AnyType:$var, AnyType:$value);
  let results = (outs);

  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// TraitDefOp
//===----------------------------------------------------------------------===//

def EmitRust_TraitDefOp : EmitRust_Op<"trait_def", [
    HasParent<"::mlir::ModuleOp">, Symbol]> {
  let summary = "Trait declaring the project's external requirements";
  let description = [{
    The `emitrust.trait_def` operation declares a Rust trait whose items are
    ASSOCIATED FUNCTIONS (no `self` receiver). It exists for exactly one
    purpose (FR-52): to express the set of symbols a translated project
    REFERENCES but no translation unit DEFINES, as a requirement on the
    crate's environment rather than as a whole-program error.

    The method names and their signatures are two arrays of the same length.
    Names must be non-empty and unique, and every entry of `fn_types` must be
    a `FunctionType` with at most one result — the same arity the Rust
    emitter accepts for `emitrust.func`.

    A receiver-less shape is deliberate. A trait method with a `self`
    parameter would force every translated call site to thread a receiver
    value through signatures that have none in C; associated functions
    monomorphize to a direct call, need no value at all, and keep the
    emitted signatures within one type parameter of what they were.

    Example:

    ```mlir
    emitrust.trait_def @Externals ["host_scale"] [(i32) -> i32]
    ```
    ```rust
    // Code emitted for the operation above.
    pub trait Externals {
        fn host_scale(v0: i32) -> i32;
    }
    ```
  }];

  let arguments = (ins SymbolNameAttr:$sym_name,
                       StrArrayAttr:$fn_names,
                       TypeArrayAttr:$fn_types);
  let results = (outs);

  let assemblyFormat = "$sym_name $fn_names $fn_types attr-dict";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// StructDefOp
//===----------------------------------------------------------------------===//

def EmitRust_StructDefOp : EmitRust_Op<"struct_def", [
    HasParent<"::mlir::ModuleOp">, Symbol]> {
  let summary = "Struct definition operation";
  let description = [{
    The `emitrust.struct_def` operation defines a named Rust struct at
    module level. The field names and field types are given as two arrays of
    the same length; the arrays may be empty (C's `struct T {};`), in which
    case the struct is emitted with an empty brace body (`struct T {}`), a
    zero-sized type in Rust just as GCC gives the empty C struct size zero.
    Names must be non-empty and unique, and every
    field type must be a scalar (integer, index, or float), an
    `!emitrust.array`, an `!emitrust.struct`, an `!emitrust.enum`, or an
    `!emitrust.fn_ptr` (no lvalues or references). Values of the struct are
    referenced through the `!emitrust.struct` type carrying the same name.

    Every value of the struct is `Copy + Default`, which the emitted item
    guarantees unconditionally: all permitted field types are themselves
    `Copy + Default` (`!emitrust.fn_ptr` renders `Option<fn(...)>`, whose
    default is `None`, and a nested struct or enum carries its own
    `Default`).

    `Default` comes from the derive whenever the derive can supply it, and
    from an explicit `impl Default` otherwise. The one thing the derive
    cannot cover is a long array field: the standard library's blanket
    `impl<T: Default> Default for [T; N]` stops at `N == 32`, so
    `#[derive(Default)]` on a struct holding, say, a `[u8; 64]` does not
    compile (FR-55). Such a struct is emitted with `Default` dropped from
    the derive list and a hand-written `impl Default` after the item whose
    body names every field with that field's default value — exactly the
    values the derive would have produced, field by field. The rule looks
    only through array nesting: a long array inside a NAMED struct field is
    already covered by that struct's own `Default`.

    Example:

    ```mlir
    emitrust.struct_def @Point ["x", "y"] [i32, i32]
    ```
    ```rust
    // Code emitted for the operation above.
    #[derive(Clone, Copy, Default)]
    struct Point {
        x: i32,
        y: i32,
    }
    ```

    ```mlir
    emitrust.struct_def @Block ["buf"] [!emitrust.array<64xui8>]
    ```
    ```rust
    // Code emitted for the operation above.
    #[derive(Clone, Copy)]
    struct Block {
        buf: [u8; 64],
    }
    impl Default for Block {
        fn default() -> Block {
            Block {
                buf: [0; 64],
            }
        }
    }
    ```
  }];

  let arguments = (ins SymbolNameAttr:$sym_name,
                       StrArrayAttr:$field_names,
                       TypeArrayAttr:$field_types);
  let results = (outs);

  let assemblyFormat = "$sym_name $field_names $field_types attr-dict";
  let hasVerifier = 1;

  let extraClassDeclaration = [{
    /// Resolves the struct definition named `name` visible from `from`.
    /// Definitions are module children (HasParent), but a consumer may sit
    /// inside a nested symbol table (`emitrust.impl` holds its methods'
    /// symbols — e.g. the actor plan stages an exported struct global's
    /// aggregate initializer in a variable inside the owner's new()), so
    /// the lookup runs in the enclosing MODULE's table, not the nearest
    /// one. Returns null when no such definition exists or the symbol is
    /// of a different kind. Mirrors DataEnumDefOp/GlobalOp::lookupFrom.
    static StructDefOp lookupFrom(mlir::Operation *from,
                                  llvm::StringRef name);
  }];
}

//===----------------------------------------------------------------------===//
// EnumDefOp
//===----------------------------------------------------------------------===//

def EmitRust_EnumDefOp : EmitRust_Op<"enum_def", [
    HasParent<"::mlir::ModuleOp">, Symbol]> {
  let summary = "Enum definition operation";
  let description = [{
    The `emitrust.enum_def` operation defines a named C enum at module
    level. The variant names and variant values are given as two arrays of
    the same non-zero length; names must be non-empty and unique and are
    emitted verbatim (the importer supplies valid Rust identifiers), values
    must be unique and within the `i32` range. Values of the enum are
    referenced through the `!emitrust.enum` type carrying the same name.

    A C enum object holds any value of its underlying integer type, not
    just the declared enumerators (C99 6.7.2.2), so the definition is
    emitted as a value-preserving open enum: a `#[repr(transparent)]`
    tuple struct wrapping the storage integer, with one associated
    constant per declared enumerator and an explicit `Default`
    implementation returning the first variant (mirroring the previous
    fieldless-enum default). The storage type is `i32`, or `u32` with the
    `unsigned_underlying` marker (set by the importer when clang assigns
    the C enum an unsigned underlying type, which requires every variant
    value to be non-negative).

    This op models the OPEN C enum only and must not grow payloads: the
    CLOSED Rust data-carrying enum — a real `enum` item with exhaustive
    matching and no out-of-range values — is `emitrust.data_enum_def`, a
    deliberately separate op with its own `!emitrust.data_enum` type, so
    the two contracts can never mix.

    Example:

    ```mlir
    emitrust.enum_def @Color ["Red", "Green", "Blue"] [0, 1, 2]
        {unsigned_underlying}
    ```
    ```rust
    // Code emitted for the operation above.
    #[repr(transparent)]
    #[derive(Clone, Copy, PartialEq)]
    struct Color(u32);
    impl Color {
        const Red: Color = Color(0);
        const Green: Color = Color(1);
        const Blue: Color = Color(2);
    }
    impl Default for Color {
        fn default() -> Color { Color::Red }
    }
    ```
  }];

  let arguments = (ins SymbolNameAttr:$sym_name,
                       StrArrayAttr:$variant_names,
                       DenseI64ArrayAttr:$variant_values,
                       UnitAttr:$unsigned_underlying);
  let results = (outs);

  let assemblyFormat = "$sym_name $variant_names $variant_values attr-dict";
  let hasVerifier = 1;

  let extraClassDeclaration = [{
    /// Resolves the enum definition named `name` visible from `from`.
    /// Definitions are module children (HasParent), but a consumer may sit
    /// inside a nested symbol table (`emitrust.impl` holds its methods'
    /// symbols, so a cast-to-enum inside any C++ method would never see a
    /// module-level enum_def through a nearest-table lookup -- FR-113), so
    /// the lookup runs in the enclosing MODULE's table, not the nearest
    /// one. Returns null when no such definition exists or the symbol is
    /// of a different kind. Mirrors StructDefOp/DataEnumDefOp/GlobalOp::
    /// lookupFrom (the FR-84 pattern).
    static EnumDefOp lookupFrom(mlir::Operation *from,
                                llvm::StringRef name);
  }];
}

//===----------------------------------------------------------------------===//
// DataEnumDefOp
//===----------------------------------------------------------------------===//

def EmitRust_DataEnumDefOp : EmitRust_Op<"data_enum_def", [
    HasParent<"::mlir::ModuleOp">, Symbol]> {
  let summary = "Closed data-enum definition operation";
  let description = [{
    The `emitrust.data_enum_def` operation defines a named CLOSED Rust
    enum at module level — a real `enum` item whose values are exactly the
    declared variants, each optionally carrying named payload fields. It
    is the message-reification surface for the FR-62 actor flavors and,
    on its own merit, the data-carrying-enum surface (tagged unions,
    Option-like shapes). Contrast `emitrust.enum_def`, which models the
    OPEN value-preserving C enum as a `#[repr(transparent)]` tuple struct;
    the two ops must never mix, and each carries its own type
    (`!emitrust.data_enum` here, `!emitrust.enum` there).

    Variants are given as a name array plus two parallel per-variant
    arrays: a list of field-name lists and a list of field-type lists. A
    variant with no fields is a unit variant. Variant names must be
    non-empty and unique; field names must be non-empty and unique within
    their variant; field types come from the `emitrust.struct_def` field
    set (scalar, array, struct, OPEN enum, fn_ptr — every one `Copy`).
    Names are emitted verbatim: the producer supplies UpperCamel variant
    names and snake_case field names, the spellings the deny-manifest
    naming lints accept.

    The item derives `Clone, Copy` only. No `Default`: unlike
    `emitrust.struct_def`, whose unconditional `Default` mirrors C's
    zero-initialization of any struct object, a closed enum has no
    canonical default variant, and nothing in the surface consumes one —
    so none is invented (should a consumer ever require it, the pinned
    fallback is the first variant when that variant is a unit variant).
    No `PartialEq`: a struct payload field derives none, so the guarantee
    could not be unconditional; `emitrust.cmp` rejects data-enum operands
    instead. Values are constructed by `emitrust.enum_variant` and
    consumed by `emitrust.match`; the type must not appear as a
    `struct_def` field or a default-initialized `emitrust.variable`.

    Example:

    ```mlir
    emitrust.data_enum_def @Msg ["Quit", "Move"] [[], ["x", "y"]]
        [[], [i32, i32]]
    ```
    ```rust
    // Code emitted for the operation above.
    #[derive(Clone, Copy)]
    enum Msg {
        Quit,
        Move { x: i32, y: i32 },
    }
    ```
  }];

  let arguments = (ins SymbolNameAttr:$sym_name,
                       StrArrayAttr:$variant_names,
                       ArrayAttr:$variant_field_names,
                       ArrayAttr:$variant_field_types);
  let results = (outs);

  let assemblyFormat =
      "$sym_name $variant_names $variant_field_names $variant_field_types "
      "attr-dict";
  let hasVerifier = 1;

  let extraClassDeclaration = [{
    /// Resolves the data-enum definition named `name` visible from `from`.
    /// Definitions are module children (HasParent), but a consumer may sit
    /// inside a nested symbol table (`emitrust.impl` holds its methods'
    /// symbols), so the lookup runs in the enclosing MODULE's table, not
    /// the nearest one. Returns null when no such definition exists or the
    /// symbol is of a different kind.
    static DataEnumDefOp lookupFrom(mlir::Operation *from,
                                    llvm::StringRef name);

    /// Returns the declaration index of the variant named `name`, or
    /// std::nullopt when no such variant exists. Only valid on verified ops.
    std::optional<unsigned> variantIndex(llvm::StringRef name);

    /// Returns the field-name / field-type list of the variant at `index`.
    /// Only valid on verified ops (the verifier pins the nested array
    /// shapes these accessors cast into).
    mlir::ArrayAttr variantFieldNames(unsigned index);
    mlir::ArrayAttr variantFieldTypes(unsigned index);
  }];
}

//===----------------------------------------------------------------------===//
// EnumVariantOp
//===----------------------------------------------------------------------===//

def EmitRust_EnumVariantOp : EmitRust_Op<"enum_variant"> {
  let summary = "Closed data-enum variant construction operation";
  let description = [{
    The `emitrust.enum_variant` operation constructs a value of a closed
    data enum: it references a visible `emitrust.data_enum_def` and one of
    its variants by name, takes exactly one operand per payload field of
    that variant (types verified against the definition through the symbol
    table), and produces the `!emitrust.data_enum` value. It renders as
    the variant literal — `Name::Variant { field: value, ... }`, or the
    bare `Name::Variant` path for a unit variant — as a `let` binding, and
    participates in the FR-61a tail fold exactly like the other
    let-producing expression ops (`emitrust.call_opaque`,
    `emitrust.method_call`). It stays OUT of the FR-61d single-use
    inlining set on purpose: Rust's expression grammar forbids a brace
    variant literal in `if`/`match` scrutinee position, and the
    never-parenthesized Cond rendering could not protect it.

    Example:

    ```mlir
    %m = emitrust.enum_variant @Msg "Move"(%x, %y)
        : (i32, i32) -> !emitrust.data_enum<"Msg">
    ```
    ```rust
    // Code emitted for the operation above.
    let v2: Msg = Msg::Move { x: v0, y: v1 };
    ```
  }];

  let arguments = (ins FlatSymbolRefAttr:$enum_def, StrAttr:$variant,
                       Variadic<AnyType>:$args);
  let results = (outs EmitRust_DataEnumType:$result);

  let assemblyFormat = [{
    $enum_def $variant `(` $args `)` attr-dict
    `:` functional-type($args, $result)
  }];
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// ActorRuntimeOp
//===----------------------------------------------------------------------===//

def EmitRust_ActorRuntimeOp : EmitRust_Op<"actor_runtime", [
    HasParent<"::mlir::ModuleOp">]> {
  let summary = "Actor runtime anchor: synthesize a mailbox runtime";
  let description = [{
    The `emitrust.actor_runtime` operation anchors the FR-62 slice-5b
    runtime synthesis for one lifted actor: the emitter derives the whole
    message surface from the referenced struct's `emitrust.impl` — a
    message enum (variant = UpperCamel(method), payload fields = the
    method's parameters via `emitrust.param_names`, plus a per-call typed
    reply channel `std::sync::mpsc::Sender<R>`, `Sender<()>` for a void
    method), a `<Actor>Handle` alias of the shared `actor_rt::Handle<Msg>`,
    a `spawn` associated function holding the mailbox loop (one match arm
    per method, delegating to the existing method on the moved state and
    replying), and one wrapper per method on the Handle's inherent impl.
    The shared `mod actor_rt` runtime (Handle/call/reap/shutdown) is a
    fixed epilogue the emitter appends once per module when any
    `emitrust.actor_runtime` op exists.

    The op carries NO reified message enum on purpose (the SLICE-5b SPIKE's
    B-prime verdict): a reply channel field is `Clone`-not-`Copy`, which
    `emitrust.data_enum_def`'s unconditional Copy derive cannot carry
    without moving the failure to rustc, and the enum is 100% mechanically
    derivable from the impl, so reification would add a verifier obligation
    with no information.

    The handle value stays `!emitrust.opaque<"<Actor>Handle">` (the type
    decision recorded in design.md): the emitter's opaque-receiver
    mutability classifier resolves handle method calls to `&mut self`
    through the module's anchor ops, so no new type crosses the surface.

    The verifier pins the sendable contract: the actor symbol resolves to
    an `emitrust.struct_def` with a non-empty `emitrust.impl`, every impl
    method is a named-parameter receiver method whose parameter and result
    types come from the struct-field validity set (every one `Copy + Send`),
    and no function outside the actor's impl holds an
    `!emitrust.mut_ref` of the actor's struct — state behind a mailbox has
    no cross-thread borrows.

    Example:

    ```mlir
    emitrust.actor_runtime @CounterActor mode = threaded
    ```
  }];

  let arguments = (ins FlatSymbolRefAttr:$actor,
                       EmitRust_ActorModeAttr:$mode);
  let results = (outs);

  let assemblyFormat = "$actor `mode` `=` $mode attr-dict";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// GlobalOp
//===----------------------------------------------------------------------===//

def EmitRust_GlobalOp : EmitRust_Op<"global", [
    HasParent<"::mlir::ModuleOp">, Symbol]> {
  let summary = "Global variable definition operation";
  let description = [{
    The `emitrust.global` operation defines a named module-level variable
    with static storage duration. The value type must be a scalar (integer,
    index, or float), an `!emitrust.array`, an `!emitrust.struct`, or an
    `!emitrust.fn_ptr`. A scalar global's optional initializer must be a
    typed attribute whose type equals the value type; an `!emitrust.fn_ptr`
    global takes an opaque attribute (such as `Some(f)`) emitted verbatim;
    an array or struct global takes an ArrayAttr with exactly one entry per
    array element (rendered `[e0, e1, ...]`) or per struct field in
    declaration order (rendered `Name { f0: e0, ... }`), recursively
    admitting nested ArrayAttr entries for aggregate elements. Without an
    initializer the global starts at the type's default value (`0`, `0.0`,
    `false`, `Name::default()`, `None`, or `[<element-default>; N]`), which
    models C's zero-initialization of static storage.

    A global carrying the `const` marker is never written; it is emitted as
    a plain Rust `static` item read directly. Because a plain `static`
    requires a const-evaluable initializer, the `const` marker is limited to
    scalar and array value types (a struct's `default()` is not const) --
    except on an initializer-less declaration-only global carrying the
    `emitrust.extern_decl` or `emitrust.external_requirement` marker, which
    is never rendered as storage at all: there the marker records the C
    const fact, and the store verifier turns it into an enforced
    getter-only contract for FR-79's const-struct requirements.
    A global without the marker is mutable; to stay free of `unsafe` it is
    emitted as a `thread_local!` `std::cell::Cell` accessed through
    `emitrust.global_load` and `emitrust.global_store`. The emitted code is
    exact for single-threaded programs (the imported C subset has no
    threads).

    Example:

    ```mlir
    emitrust.global @counter <0 : i32> : i32
    emitrust.global const @limit <100 : i32> : i32
    emitrust.global @table : !emitrust.array<4xi32>
    emitrust.global const @pair <[1 : i32, 2 : i32]> : !emitrust.array<2xi32>
    ```
    ```rust
    // Code emitted for the operations above.
    thread_local! {
        static counter: std::cell::Cell<i32> = std::cell::Cell::new(0);
    }
    static limit: i32 = 100;
    thread_local! {
        static table: std::cell::Cell<[i32; 4]> = std::cell::Cell::new([0; 4]);
    }
    static pair: [i32; 2] = [1, 2];
    ```
  }];

  let arguments = (ins SymbolNameAttr:$sym_name,
                       TypeAttrOf<AnyType>:$type,
                       OptionalAttr<EmitRust_InitAttr>:$init,
                       UnitAttr:$is_const);
  let results = (outs);

  let assemblyFormat =
      "(`const` $is_const^)? $sym_name (` ` `<` $init^ `>`)? attr-dict `:` $type";
  let hasVerifier = 1;

  let extraClassDeclaration = [{
    /// Resolves the global named `name` visible from `from`. Globals are
    /// module children, but an accessor may live inside a nested symbol
    /// table (`emitrust.impl` holds the SymbolTable trait and FR-30 owner
    /// promotion moves functions into impl blocks), so the lookup runs in
    /// the enclosing MODULE's table, not the nearest one. Returns null
    /// when no such global exists or the symbol is of a different kind.
    static GlobalOp lookupFrom(mlir::Operation *from, llvm::StringRef name);
  }];
}

//===----------------------------------------------------------------------===//
// GlobalLoadOp
//===----------------------------------------------------------------------===//

def EmitRust_GlobalLoadOp : EmitRust_Op<"global_load",
    [DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Global variable read operation";
  let description = [{
    The `emitrust.global_load` operation reads the current whole value of an
    `emitrust.global`. The symbol must reference a global in the enclosing
    module and the result type must equal the global's value type. A load
    from a `const` global is emitted as a direct read of the static item;
    a load from a mutable global is emitted as a `Cell` read through the
    thread-local's `with` accessor.

    Example:

    ```mlir
    %v = emitrust.global_load @counter : i32
    ```
    ```rust
    // Code emitted for the operation above.
    let v0: i32 = counter.with(|c| c.get());
    ```
  }];

  let arguments = (ins FlatSymbolRefAttr:$global);
  let results = (outs AnyType:$result);

  let assemblyFormat = "$global attr-dict `:` type($result)";
}

//===----------------------------------------------------------------------===//
// GlobalStoreOp
//===----------------------------------------------------------------------===//

def EmitRust_GlobalStoreOp : EmitRust_Op<"global_store",
    [DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Global variable write operation";
  let description = [{
    The `emitrust.global_store` operation replaces the whole value of an
    `emitrust.global`. The symbol must reference a non-`const` global in the
    enclosing module and the value type must equal the global's value type;
    stores to `const` globals are rejected by the verifier. The operation is
    emitted as a `Cell` write through the thread-local's `with` accessor.
    Partial updates (an element of a global array or a field of a global
    struct) are modeled as load-modify-store of the whole value, which is
    exact for single-threaded programs.

    Example:

    ```mlir
    emitrust.global_store %v, @counter : i32
    ```
    ```rust
    // Code emitted for the operation above.
    counter.with(|c| c.set(v0));
    ```
  }];

  let arguments = (ins AnyType:$value, FlatSymbolRefAttr:$global);
  let results = (outs);

  let assemblyFormat = "$value `,` $global attr-dict `:` type($value)";
}

//===----------------------------------------------------------------------===//
// GlobalAddrOp
//===----------------------------------------------------------------------===//

def EmitRust_GlobalAddrOp : EmitRust_Op<"global_addr",
    [DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Address of a const global as a shared reference";
  let description = [{
    The `emitrust.global_addr` operation yields the address of a `const`
    `emitrust.global` as a shared reference whose pointee is the global's
    value type. It is the FR-80 pre-lowering anchor for an ADDRESS-CARRYING
    external requirement: the C importer emits it for `&g` on a qualifying
    extern const struct, and `emitrust-lower-external-requirements` rewrites
    it into the requirement's `E::<getter>()` call, whose
    `() -> !emitrust.ref<T>` item renders `fn getter() -> &'static T` —
    the consumer's one static item, so every rewritten address is the SAME
    stable address and pointer identity is preserved.

    Only `const` globals qualify structurally: a mutable global's storage
    is a thread-local `Cell` with no lendable `&T`. The op has NO Rust
    rendering of its own — an instance that survives to emission was never
    resolved into a requirement, and the emitter refuses it loudly (the
    FR-52 marker contract).

    Example:

    ```mlir
    %r = emitrust.global_addr @cfg : !emitrust.ref<!emitrust.struct<"S">>
    ```
  }];

  // The result is AnyType in ODS (like global_load's) so the assembly
  // prints the full `!emitrust.ref<...>` spelling; the verifier enforces
  // the shared-reference shape.
  let arguments = (ins FlatSymbolRefAttr:$global);
  let results = (outs AnyType:$result);

  let assemblyFormat = "$global attr-dict `:` type($result)";
}

//===----------------------------------------------------------------------===//
// CellGetOp
//===----------------------------------------------------------------------===//

def EmitRust_CellGetOp : EmitRust_Op<"cell_get"> {
  let summary = "Cell-slice element read operation";
  let description = [{
    The `emitrust.cell_get` operation reads one element of a cell-slice
    reference at an i64 index. The operand is the
    `!emitrust.ref<!emitrust.cell_slice<T>>` reference itself — no
    dereference or lvalue staging is involved — and the result type must
    equal the cell-slice element type. The operation is emitted as a
    `Cell::get` call on the indexed element.

    Example:

    ```mlir
    %v = emitrust.cell_get %s[%i]
        : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64) -> i32
    ```
    ```rust
    // Code emitted for the operation above.
    let v2: i32 = v0[v1 as usize].get();
    ```
  }];

  let arguments = (ins EmitRust_RefType:$slice, I64:$index);
  let results = (outs AnyType:$result);

  let assemblyFormat =
      "$slice `[` $index `]` attr-dict `:` functional-type(operands, results)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// CellSetOp
//===----------------------------------------------------------------------===//

def EmitRust_CellSetOp : EmitRust_Op<"cell_set"> {
  let summary = "Cell-slice element write operation";
  let description = [{
    The `emitrust.cell_set` operation replaces one element of a cell-slice
    reference at an i64 index. The operand is the shared
    `!emitrust.ref<!emitrust.cell_slice<T>>` reference itself (Cells give
    interior mutability, so a shared reference suffices) and the value type
    must equal the cell-slice element type. The operation is emitted as a
    `Cell::set` call on the indexed element; the write is immediately
    visible to `emitrust.global_load` of the backing global because both
    touch the same thread-local `Cell` storage.

    Example:

    ```mlir
    emitrust.cell_set %s[%i], %v
        : (!emitrust.ref<!emitrust.cell_slice<i32>>, i64, i32) -> ()
    ```
    ```rust
    // Code emitted for the operation above.
    v0[v1 as usize].set(v2);
    ```
  }];

  let arguments = (ins EmitRust_RefType:$slice, I64:$index, AnyType:$value);
  let results = (outs);

  let assemblyFormat = "$slice `[` $index `]` `,` $value attr-dict `:` "
                       "functional-type(operands, results)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// GlobalCellsOp
//===----------------------------------------------------------------------===//

def EmitRust_GlobalCellsOp : EmitRust_Op<"global_cells",
    [DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Global cell-slice borrow region operation";
  let description = [{
    The `emitrust.global_cells` operation borrows a mutable
    `emitrust.global` of array type as a cell-slice for the extent of its
    single-block region. The region's entry block takes one argument, the
    borrowed `!emitrust.ref<!emitrust.cell_slice<T>>` whose element type
    `T` equals the global's array element type, and terminates with
    `emitrust.yield`. The operation is a statement: it takes no operands
    and produces no results, so no borrow ever escapes its accessor. Call
    sites taking several distinct global arguments nest one region per
    global, leftmost argument outermost.

    The operation is emitted as the thread-local global's `with` accessor:
    the closure binds the `&Cell<[T; N]>`, coerces it to the unsized
    `&Cell<[T]>`, and flattens it with `as_slice_of_cells` into the
    region argument's `&[std::cell::Cell<T>]` value.

    Example:

    ```mlir
    emitrust.global_cells @g {
    ^bb0(%cells: !emitrust.ref<!emitrust.cell_slice<i32>>):
      emitrust.yield
    }
    ```
    ```rust
    // Code emitted for the operation above.
    g.with(|__emitrust_tl| {
        let __emitrust_tl: &std::cell::Cell<[i32]> = __emitrust_tl;
        let v0: &[std::cell::Cell<i32>] = __emitrust_tl.as_slice_of_cells();
    });
    ```
  }];

  let arguments = (ins FlatSymbolRefAttr:$global);
  let results = (outs);
  let regions = (region SizedRegion<1>:$body);

  let assemblyFormat = "$global attr-dict-with-keyword $body";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// VariableOp
//===----------------------------------------------------------------------===//

def EmitRust_VariableOp : EmitRust_Op<"variable"> {
  let summary = "Local variable declaration operation";
  let description = [{
    The `emitrust.variable` operation declares a local variable and produces
    the lvalue naming its storage. A scalar variable's optional initializer
    must be a typed attribute whose type equals the lvalue's wrapped value
    type; an array variable additionally takes an ArrayAttr with exactly one
    entry per element (rendered `[e0, e1, ...]`, recursively admitting
    nested ArrayAttr entries). Without an initializer, the variable is
    emitted with the type's default value: `0` for integers and index, `0.0`
    for floats, `false` for `i1`, `Name::default()` for structs,
    `[<element-default>; N]` for arrays, and `String::new()` for the opaque
    `String` type (the staging binding of the sprintf lowering).

    A variable carrying the `const` marker is never written after its
    initializer; it is emitted as an immutable `let` binding (the backing
    of a C string literal, for example). A variable without the marker is
    the ordinary mutable `let mut` binding.

    A decl-bound variable may carry its FINAL Rust base spelling in the
    `named` clause (FR-61e); the importer computes it (snake_cased under
    the idiomatic rename, `_`-suffixed for Rust keywords), and the emitter
    binds under that spelling -- `_`-prefixed when never read, uniquified
    per function -- instead of the generated vN. Synthesized temporaries
    carry no name.

    Example:

    ```mlir
    %0 = emitrust.variable <42 : i32> : !emitrust.lvalue<i32>
    %1 = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
    %2 = emitrust.variable const <[104 : i8, 0 : i8]>
        : !emitrust.lvalue<!emitrust.array<2xi8>>
    %3 = emitrust.variable named "count" <1 : i32> : !emitrust.lvalue<i32>
    ```
    ```rust
    // Code emitted for the operations above.
    let mut v0: i32 = 42;
    let mut v1: [i32; 4] = [0; 4];
    let v2: [i8; 2] = [104, 0];
    let mut count: i32 = 1;
    ```
  }];

  let arguments = (ins OptionalAttr<EmitRust_InitAttr>:$init,
                       UnitAttr:$is_const,
                       OptionalAttr<StrAttr>:$c_name);
  let results = (outs EmitRust_LValueType:$result);

  let builders = [
    OpBuilder<(ins "Type":$resultType, CArg<"Attribute", "{}">:$init,
                   CArg<"bool", "false">:$isConst,
                   CArg<"llvm::StringRef", "{}">:$cName), [{
      if (init)
        $_state.addAttribute(getInitAttrName($_state.name), init);
      if (isConst)
        $_state.addAttribute(getIsConstAttrName($_state.name),
                             $_builder.getUnitAttr());
      if (!cName.empty())
        $_state.addAttribute(getCNameAttrName($_state.name),
                             $_builder.getStringAttr(cName));
      $_state.addTypes(resultType);
    }]>
  ];
  let skipDefaultBuilders = 1;

  let assemblyFormat =
      "(`named` $c_name^)? (`const` $is_const^)? (` ` `<` $init^ `>`)? "
      "attr-dict `:` qualified(type($result))";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Container ops (the backend-agnostic container computational model)
//===----------------------------------------------------------------------===//
// The backend-agnostic "container computational model": nullable indices into
// a collection of `T`. One high-level representation, lowered by the
// emitrust-lower-containers pass to a concrete container. The production
// backend is a fixed `[T;CAP]` array + i64 free cursor (the FR-39 node-pool
// shape); a growable `Vec<T>` backend is deferred to the heap RFC (design.md
// W4.5). The collection place holds a placeholder
// `!emitrust.opaque<"__emitrust_collection">` value type until the pass
// replaces it with the concrete pool lvalue. `capacity` is the AST-recovered
// folded trip count, required by the array backend to size the pool.
//
// The importer emits these three ops at malloc-pool recognition time; the
// lowering pass runs first in the emitrust-cc pipeline (before mem2reg), so
// everything downstream sees the concrete pool exactly as if the importer had
// inlined it.

def EmitRust_CollectionOp : EmitRust_Op<"collection"> {
  let summary = "High-level backend-agnostic collection place";
  let arguments = (ins TypeAttrOf<AnyType>:$element_type,
                       OptionalAttr<I64Attr>:$capacity);
  let results = (outs EmitRust_LValueType:$result);
  let assemblyFormat = "attr-dict `:` qualified(type($result))";
}

def EmitRust_CollectionPushOp : EmitRust_Op<"collection_push"> {
  let summary = "Append a defaulted element, return its index";
  let arguments = (ins EmitRust_LValueType:$collection);
  let results = (outs I64:$index);
  let assemblyFormat =
      "$collection attr-dict `:` functional-type(operands, results)";
}

def EmitRust_CollectionAtOp : EmitRust_Op<"collection_at"> {
  let summary = "Element place at an index into the collection";
  let arguments = (ins EmitRust_LValueType:$collection, I64:$index);
  let results = (outs EmitRust_LValueType:$result);
  let assemblyFormat =
      "$collection `[` $index `]` attr-dict `:` "
      "functional-type(operands, results)";
}

//===----------------------------------------------------------------------===//
// MemberOp
//===----------------------------------------------------------------------===//

def EmitRust_MemberOp : EmitRust_Op<"member"> {
  let summary = "Struct member place operation";
  let description = [{
    The `emitrust.member` operation refines a struct-typed lvalue into the
    lvalue of one of its fields. The operand's wrapped value type must be an
    `!emitrust.struct`; the named field is not cross-checked against the
    corresponding `emitrust.struct_def`. The operation emits no code at its
    program point; it contributes `.member` to the place expression of any
    load, assign, or borrow that consumes it.

    Example:

    ```mlir
    %x = emitrust.member %p["x"]
        : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
    %v = emitrust.load %x : (!emitrust.lvalue<i32>) -> i32
    ```
    ```rust
    // Code emitted for the operations above.
    let v1: i32 = v0.x;
    ```
  }];

  let arguments = (ins EmitRust_LValueType:$operand, StrAttr:$member);
  let results = (outs EmitRust_LValueType:$result);

  let assemblyFormat =
      "$operand `[` $member `]` attr-dict `:` functional-type($operand, $result)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// EnumRawOp
//===----------------------------------------------------------------------===//

def EmitRust_EnumRawOp : EmitRust_Op<"enum_raw",
    [DeclareOpInterfaceMethods<InferIntRangeInterface,
                               ["inferResultRanges"]>]> {
  let summary = "Enum raw-representation place operation";
  let description = [{
    The `emitrust.enum_raw` operation refines an enum-typed lvalue into the
    lvalue of its raw integer representation. The operand's wrapped value
    type must be an `!emitrust.enum` and the result's wrapped value type a
    32-bit integer (the enum's storage type, `i32` or `ui32`). Because the
    emitted enum is a `#[repr(transparent)]` tuple struct over its storage
    integer, the operation emits no code at its program point; it
    contributes `.0` to the place expression of any load, assign, or borrow
    that consumes it. The importer uses it for C's enum/underlying-type
    pointer compatibility (`unsigned int *p = &e;`-shaped call arguments).

    The operation implements `InferIntRangeInterface` conservatively: its
    result is an lvalue, not an integer SSA value, so no range is inferred.

    Example:

    ```mlir
    %raw = emitrust.enum_raw %e
        : (!emitrust.lvalue<!emitrust.enum<"Color">>) -> !emitrust.lvalue<ui32>
    %r = emitrust.addr_of mut %raw
        : (!emitrust.lvalue<ui32>) -> !emitrust.mut_ref<ui32>
    ```
    ```rust
    // Code emitted for the operations above.
    let v1: &mut u32 = &mut v0.0;
    ```
  }];

  let arguments = (ins EmitRust_LValueType:$operand);
  let results = (outs EmitRust_LValueType:$result);

  let assemblyFormat =
      "$operand attr-dict `:` functional-type($operand, $result)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// SubscriptOp
//===----------------------------------------------------------------------===//

def EmitRust_SubscriptOp : EmitRust_Op<"subscript"> {
  let summary = "Array or slice subscript place operation";
  let description = [{
    The `emitrust.subscript` operation refines an array- or slice-typed
    lvalue into the lvalue of one of its elements. The operand's wrapped
    value type must be an `!emitrust.array` or `!emitrust.slice` whose
    element type equals the result lvalue's wrapped value type. The
    operation emits no code at its program point; it contributes
    `[index as usize]` to the place expression of any load, assign, or
    borrow that consumes it (the `as usize` cast is omitted when the index
    is of `index` type).

    W2.3: the operand's wrapped value type may also be an `!emitrust.opaque`
    type (the `std::vector<T>` recognition surface, rendered `Vec<T>` —
    `operator[]`/`at()` both map to this same bracket-indexing place, the
    UB-refinement stance being that both panic on out-of-bounds exactly
    like Rust's `Index`/`IndexMut`). The opaque case is NOT structurally
    cross-checked against a decomposable element type (there is none to
    derive from an opaque string) — the caller is trusted to supply a
    result value type consistent with the opaque's element, mirroring
    `emitrust.call_opaque`'s trust model for opaque interop.

    Example:

    ```mlir
    %e = emitrust.subscript %a[%i]
        : (!emitrust.lvalue<!emitrust.array<4xi32>>, index) -> !emitrust.lvalue<i32>
    %v = emitrust.load %e : (!emitrust.lvalue<i32>) -> i32
    ```
    ```rust
    // Code emitted for the operations above.
    let v2: i32 = v0[v1];
    ```
  }];

  let arguments = (ins EmitRust_LValueType:$array,
                       AnyTypeOf<[AnyInteger, Index]>:$index);
  let results = (outs EmitRust_LValueType:$result);

  let assemblyFormat =
      "$array `[` $index `]` attr-dict `:` functional-type(operands, results)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// DerefOp
//===----------------------------------------------------------------------===//

def EmitRust_DerefOp : EmitRust_Op<"deref"> {
  let summary = "Reference dereference place operation";
  let description = [{
    The `emitrust.deref` operation turns a shared or mutable reference into
    the lvalue of its pointee. The reference's pointee type must equal the
    result lvalue's wrapped value type. The operation emits no code at its
    program point; it contributes `(*ref)` to the place expression of any
    load, assign, or borrow that consumes it.

    Example:

    ```mlir
    %p = emitrust.deref %r : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>
    emitrust.assign %p = %v : !emitrust.lvalue<i32>
    ```
    ```rust
    // Code emitted for the operations above.
    (*v0) = v1;
    ```
  }];

  let arguments =
      (ins AnyTypeOf<[EmitRust_RefType, EmitRust_MutRefType]>:$operand);
  let results = (outs EmitRust_LValueType:$result);

  let assemblyFormat =
      "$operand attr-dict `:` functional-type($operand, $result)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// LoadOp
//===----------------------------------------------------------------------===//

def EmitRust_LoadOp : EmitRust_Op<"load"> {
  let summary = "Load operation";
  let description = [{
    The `emitrust.load` operation reads the current value of an lvalue. The
    result type must equal the operand's wrapped value type. The operation
    is emitted as a `let` binding of the place expression.

    Example:

    ```mlir
    %v = emitrust.load %p : (!emitrust.lvalue<i32>) -> i32
    ```
    ```rust
    // Code emitted for the operation above.
    let v1: i32 = v0;
    ```
  }];

  let arguments = (ins EmitRust_LValueType:$operand);
  let results = (outs AnyType:$result);

  let assemblyFormat =
      "$operand attr-dict `:` functional-type($operand, $result)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// AddrOfOp
//===----------------------------------------------------------------------===//

def EmitRust_AddrOfOp : EmitRust_Op<"addr_of"> {
  let summary = "Borrow operation";
  let description = [{
    The `emitrust.addr_of` operation borrows an lvalue, producing a shared
    reference (`&place`) or, with the `mut` marker, a mutable reference
    (`&mut place`). The `mut` marker must be present if and only if the
    result is an `!emitrust.mut_ref`, and the reference's pointee type must
    equal the operand's wrapped value type.

    Example:

    ```mlir
    %r = emitrust.addr_of %p : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
    %m = emitrust.addr_of mut %p
        : (!emitrust.lvalue<i32>) -> !emitrust.mut_ref<i32>
    ```
    ```rust
    // Code emitted for the operations above.
    let v1: &i32 = &v0;
    let v2: &mut i32 = &mut v0;
    ```
  }];

  let arguments = (ins EmitRust_LValueType:$operand, UnitAttr:$is_mut);
  let results =
      (outs AnyTypeOf<[EmitRust_RefType, EmitRust_MutRefType]>:$result);

  let assemblyFormat =
      "(`mut` $is_mut^)? $operand attr-dict `:` functional-type($operand, $result)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// SliceOfOp
//===----------------------------------------------------------------------===//

def EmitRust_SliceOfOp : EmitRust_Op<"slice_of"> {
  let summary = "Slice borrow operation";
  let description = [{
    The `emitrust.slice_of` operation borrows the tail of an array- or
    slice-typed lvalue starting at an element index, producing a shared
    (`&place[index as usize..]`) or, with the `mut` marker, a mutable
    (`&mut place[index as usize..]`) reference to a slice of the elements
    (the `as usize` cast is omitted when the index is of `index` type).
    The base lvalue's wrapped value type must be an `!emitrust.array` or
    `!emitrust.slice` whose element type equals the result slice's element
    type, or an `!emitrust.opaque` (the FR-94 owned `Vec<u8>` FAM tail; the
    `Vec<T>` recognition surface generally), whose result element is trusted
    rather than cross-checked (the `emitrust.subscript` opaque trust model).
    The `mut` marker must be present if and only if the result is an
    `!emitrust.mut_ref` (mirroring `emitrust.addr_of`).

    Example:

    ```mlir
    %s = emitrust.slice_of mut %arr[%i]
        : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64)
          -> !emitrust.mut_ref<!emitrust.slice<i32>>
    ```
    ```rust
    // Code emitted for the operation above.
    let v2: &mut [i32] = &mut v0[v1 as usize..];
    ```
  }];

  let arguments = (ins EmitRust_LValueType:$base,
                       AnyTypeOf<[AnyInteger, Index]>:$index,
                       UnitAttr:$is_mut);
  let results =
      (outs AnyTypeOf<[EmitRust_RefType, EmitRust_MutRefType]>:$result);

  let assemblyFormat =
      "(`mut` $is_mut^)? $base `[` $index `]` attr-dict `:` functional-type(operands, results)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// ArgvArgOp
//===----------------------------------------------------------------------===//

def EmitRust_ArgvArgOp : EmitRust_Op<"argv_arg"> {
  let summary = "Argv table argument borrow operation";
  let description = [{
    The `emitrust.argv_arg` operation borrows one command-line argument's
    NUL-terminated byte run out of an `!emitrust.argv_table` value (C99-43
    C3), producing a shared `!emitrust.ref<!emitrust.slice<i8>>`. It is
    emitted as a `let` binding of the whole-vector slice borrow
    `&table[index as usize][..]` (the `as usize` cast is omitted when the
    index is of `index` type); an out-of-range index panics where the C
    read is undefined behavior, the accepted refinement direction. From
    the result, the ordinary slice surface applies: `emitrust.deref` +
    `emitrust.subscript` read single argument bytes, and the on-demand
    `__emitrust_cstr_out` printf helper consumes the slice whole.

    Example:

    ```mlir
    %s = emitrust.argv_arg %argv[%i]
        : (!emitrust.argv_table, i32) -> !emitrust.ref<!emitrust.slice<i8>>
    ```
    ```rust
    // Code emitted for the operation above.
    let v2: &[i8] = &v0[v1 as usize][..];
    ```
  }];

  let arguments = (ins EmitRust_ArgvTableType:$table,
                       AnyTypeOf<[AnyInteger, Index]>:$index);
  let results = (outs EmitRust_RefType:$result);

  let assemblyFormat =
      "$table `[` $index `]` attr-dict `:` functional-type(operands, results)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Binary arithmetic operations
//===----------------------------------------------------------------------===//

// Base class for EmitRust binary arithmetic operations. Both operands and
// the result share one type, which must be an integer, index, float, or
// opaque type.
// All binary arithmetic operations implement `InferIntRangeInterface` with
// WRAP (two's-complement) transfer functions: unsigned EmitRust arithmetic
// is emitted as Rust `wrapping_*` calls, so overflow is a defined
// wrap-around, never poison. Signedness-dependent operations (div, rem,
// shr) select the unsigned transfer function when the operand type is an
// unsigned (`ui<N>`) integer and the signed one otherwise.
class EmitRust_BinaryOp<string mnemonic, list<Trait> traits = []>
    : EmitRust_Op<mnemonic,
                  !listconcat(traits, [SameOperandsAndResultType,
                       DeclareOpInterfaceMethods<InferIntRangeInterface,
                                                 ["inferResultRanges"]>])> {
  let arguments = (ins EmitRust_ArithmeticType:$lhs,
                       EmitRust_ArithmeticType:$rhs);
  let results = (outs EmitRust_ArithmeticType:$result);

  let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

def EmitRust_AddOp : EmitRust_BinaryOp<"add"> {
  let summary = "Addition operation";
  let description = [{
    The `emitrust.add` operation is emitted as the infix binary `+` operator.
    When the result type is an unsigned integer it is emitted as the
    `wrapping_add` method call instead: C defines unsigned overflow as
    wrap-around, whereas Rust's infix `+` panics on overflow in debug
    builds.

    Example:

    ```mlir
    %r = emitrust.add %a, %b : i32
    %u = emitrust.add %c, %d : ui32
    ```
    ```rust
    // Code emitted for the operations above.
    let v3 = v1 + v2;
    let v6: u32 = v4.wrapping_add(v5);
    ```
  }];
}

def EmitRust_SubOp : EmitRust_BinaryOp<"sub"> {
  let summary = "Subtraction operation";
  let description = [{
    The `emitrust.sub` operation is emitted as the infix binary `-` operator.
    When the result type is an unsigned integer it is emitted as the
    `wrapping_sub` method call instead: C defines unsigned overflow as
    wrap-around, whereas Rust's infix `-` panics on overflow in debug
    builds.

    Example:

    ```mlir
    %r = emitrust.sub %a, %b : i32
    ```
  }];
}

def EmitRust_MulOp : EmitRust_BinaryOp<"mul"> {
  let summary = "Multiplication operation";
  let description = [{
    The `emitrust.mul` operation is emitted as the infix binary `*` operator.
    When the result type is an unsigned integer it is emitted as the
    `wrapping_mul` method call instead: C defines unsigned overflow as
    wrap-around, whereas Rust's infix `*` panics on overflow in debug
    builds.

    Example:

    ```mlir
    %r = emitrust.mul %a, %b : i32
    ```
  }];
}

def EmitRust_DivOp : EmitRust_BinaryOp<"div"> {
  let summary = "Division operation";
  let description = [{
    The `emitrust.div` operation is emitted as the infix binary `/` operator.

    Example:

    ```mlir
    %r = emitrust.div %a, %b : i32
    ```
  }];
}

def EmitRust_RemOp : EmitRust_BinaryOp<"rem"> {
  let summary = "Remainder operation";
  let description = [{
    The `emitrust.rem` operation is emitted as the infix binary `%` operator.

    Example:

    ```mlir
    %r = emitrust.rem %a, %b : i32
    ```
  }];
}

def EmitRust_AndOp : EmitRust_BinaryOp<"and"> {
  let summary = "Bitwise AND operation";
  let description = [{
    The `emitrust.and` operation is emitted as the infix binary `&` operator.
    Rust's `&` is the bitwise AND on every integer type, an exact match for
    C's `&` on matching widths.

    Example:

    ```mlir
    %r = emitrust.and %a, %b : i32
    ```
  }];
}

def EmitRust_OrOp : EmitRust_BinaryOp<"or"> {
  let summary = "Bitwise OR operation";
  let description = [{
    The `emitrust.or` operation is emitted as the infix binary `|` operator.
    Rust's `|` is the bitwise inclusive OR on every integer type, an exact
    match for C's `|` on matching widths.

    Example:

    ```mlir
    %r = emitrust.or %a, %b : i32
    ```
  }];
}

def EmitRust_XorOp : EmitRust_BinaryOp<"xor"> {
  let summary = "Bitwise XOR operation";
  let description = [{
    The `emitrust.xor` operation is emitted as the infix binary `^` operator.
    Rust's `^` is the bitwise exclusive OR on every integer type, an exact
    match for C's `^` on matching widths. C's bitwise complement `~x`
    imports as `x ^ -1`, which this operation renders exactly.

    Example:

    ```mlir
    %r = emitrust.xor %a, %b : i32
    ```
  }];
}

def EmitRust_ShlOp : EmitRust_BinaryOp<"shl"> {
  let summary = "Left shift operation";
  let description = [{
    The `emitrust.shl` operation is emitted as the infix binary `<<`
    operator. Rust's `<<` shifts in zero bits on every integer type;
    shifting by the type's width or more panics in debug builds, which is
    undefined behavior in C, so no defined C program observes a divergence.

    Example:

    ```mlir
    %r = emitrust.shl %a, %b : i32
    ```
  }];
}

def EmitRust_ShrOp : EmitRust_BinaryOp<"shr"> {
  let summary = "Right shift operation";
  let description = [{
    The `emitrust.shr` operation is emitted as the infix binary `>>`
    operator. The operand type selects the semantics exactly as in Rust:
    `>>` on a signed `iN` is an arithmetic (sign-extending) shift, and `>>`
    on an unsigned `uN` is a logical (zero-extending) shift. Producers must
    therefore pick the operand type matching the intended C semantics; the
    Arith conversion guards enforce this (shrsi only lowers on
    signless/signed types, shrui only on unsigned types).

    Example:

    ```mlir
    %r = emitrust.shr %a, %b : i32
    %u = emitrust.shr %c, %d : ui32
    ```
  }];
}

//===----------------------------------------------------------------------===//
// CmpOp
//===----------------------------------------------------------------------===//

def EmitRust_CmpOp : EmitRust_Op<"cmp",
    [SameTypeOperands,
     DeclareOpInterfaceMethods<InferIntRangeInterface,
                               ["inferResultRanges"]>]> {
  let summary = "Comparison operation";
  let description = [{
    The `emitrust.cmp` operation is emitted as an infix Rust comparison.
    Its predicate attribute defines the comparison operator:

    - equal to                 (mnemonic: `eq`; integer value: `0`)
    - not equal to             (mnemonic: `ne`; integer value: `1`)
    - less than                (mnemonic: `lt`; integer value: `2`)
    - less than or equal to    (mnemonic: `le`; integer value: `3`)
    - greater than             (mnemonic: `gt`; integer value: `4`)
    - greater than or equal to (mnemonic: `ge`; integer value: `5`)

    Operands of `!emitrust.enum` type are supported with the `eq` and `ne`
    predicates only; enums derive `PartialEq` but not `PartialOrd`, so the
    verifier rejects ordered predicates on enum operands. Operands of
    `!emitrust.fn_ptr` type follow the same rule: `Option<fn(...)>` is
    `PartialEq` (a null check compares against a `None` constant) but has
    no meaningful ordering.

    The operation implements `InferIntRangeInterface`: the `i1` result is
    `[0, 1]`, refined to a constant when the operand ranges statically
    decide the predicate. Ordered predicates compare unsigned when the
    operand type is an unsigned (`ui<N>`) integer — Rust's `<` on `uN` is
    the unsigned comparison — and signed otherwise. Non-integer operands
    leave the result at `[0, 1]`.

    Example:

    ```mlir
    %r = emitrust.cmp lt, %a, %b : (i32, i32) -> i1
    %s = emitrust.cmp eq, %c, %d
        : (!emitrust.enum<"Color">, !emitrust.enum<"Color">) -> i1
    ```
    ```rust
    // Code emitted for the operations above.
    let v3 = v1 < v2;
    let v6 = v4 == v5;
    ```
  }];

  let arguments = (ins EmitRust_CmpPredicateAttr:$predicate,
                       EmitRust_CmpOperandType:$lhs,
                       EmitRust_CmpOperandType:$rhs);
  let results = (outs I1:$result);

  let assemblyFormat =
      "$predicate `,` operands attr-dict `:` functional-type(operands, results)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// CastOp
//===----------------------------------------------------------------------===//

def EmitRust_CastOp : EmitRust_Op<"cast",
    [DeclareOpInterfaceMethods<InferIntRangeInterface,
                               ["inferResultRanges"]>]> {
  let summary = "Cast operation";
  let description = [{
    The `emitrust.cast` operation performs an explicit type conversion and
    is emitted as a Rust `as` cast expression.

    The operation implements `InferIntRangeInterface` following Rust `as`
    semantics for integer-to-integer casts: same-width casts preserve the
    bit pattern, narrowing casts truncate, and widening casts zero-extend
    from an unsigned (`ui<N>`) or boolean (`i1`) source and sign-extend
    from a signless/signed source (signless `i<N>` renders as Rust's signed
    `iN`). Casts involving non-integer types infer the full result range.

    An `!emitrust.enum` source with an integer result is an enum-to-integer
    conversion: the enum's open-enum tuple-struct representation exposes
    its raw value through field `0`, so the cast renders e.g. `.0 as i32`.
    An `!emitrust.enum` result is an integer-to-enum conversion; the
    verifier requires an integer source that is not `i1`, and the emitter
    renders the tuple-struct constructor around an `as` cast to the enum's
    storage type (value-preserving, matching C's conversion to the enum's
    underlying type). Boolean (`i1`) result types are rejected by the
    emitter (`as bool` does not exist); such conversions must be modeled
    as comparisons.

    Example:

    ```mlir
    %r = emitrust.cast %v : i32 to i64
    %s = emitrust.cast %c : !emitrust.enum<"Color"> to i32
    %t = emitrust.cast %v : i32 to !emitrust.enum<"Color">
    ```
    ```rust
    // Code emitted for the operations above.
    let v2 = v1 as i64;
    let v4 = v3.0 as i32;
    let v5 = Color(v1 as u32);
    ```
  }];

  let arguments = (ins AnyType:$source);
  let results = (outs AnyType:$result);

  let assemblyFormat = "$source attr-dict `:` type($source) `to` type($result)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// BitcastOp
//===----------------------------------------------------------------------===//

def EmitRust_BitcastOp : EmitRust_Op<"bitcast"> {
  let summary = "Bit-exact float/integer reinterpretation";
  let description = [{
    The `emitrust.bitcast` operation reinterprets the bit pattern of a
    floating-point value as a same-width integer, or vice versa, without
    any value conversion — the exact semantics of Rust's
    `f32::to_bits`/`f32::from_bits` (and the `f64` pair), which is what
    the emitter renders. This is deliberately distinct from
    `emitrust.cast`, whose `as` rendering value-converts between the
    float and integer domains.

    The verifier requires exactly one float side (`f32` or `f64`) and one
    integer side of the same bit width; the integer may be signless
    (Rust's signed `iN`) or unsigned (`uiN`). `to_bits`/`from_bits` speak
    `u32`/`u64`, so a signless integer side additionally wraps a
    same-width `as` conversion, which is itself bit-preserving.

    Example:

    ```mlir
    %f = emitrust.bitcast %bits : ui32 to f32
    %i = emitrust.bitcast %v : f32 to i32
    ```
    ```rust
    // Code emitted for the operations above.
    let v2 = f32::from_bits(v1);
    let v4 = v3.to_bits() as i32;
    ```
  }];

  let arguments = (ins AnyType:$source);
  let results = (outs AnyType:$result);

  let assemblyFormat = "$source attr-dict `:` type($source) `to` type($result)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// SelectOp
//===----------------------------------------------------------------------===//

def EmitRust_SelectOp : EmitRust_Op<"select",
    [AllTypesMatch<["trueValue", "falseValue", "result"]>,
     DeclareOpInterfaceMethods<InferIntRangeInterface,
                               ["inferResultRangesFromOptional"]>]> {
  let summary = "Select operation emitted as a Rust if expression";
  let description = [{
    The `emitrust.select` operation chooses between two values of the same
    type based on a boolean condition, without any short-circuiting: it is
    the EmitRust counterpart of `arith.select`, which the canonicalizer
    synthesizes from folded conditional chains. It is emitted as a `let`
    binding of a Rust `if` expression.

    The operation implements `InferIntRangeInterface` through
    `inferResultRangesFromOptional` (mirroring `arith.select`): a
    statically known condition forwards the matching branch's lattice
    value, otherwise the result is the union of both branch ranges.

    Example:

    ```mlir
    %r = emitrust.select %cond, %a, %b : i32
    ```
    ```rust
    // Code emitted for the operation above.
    let v3: i32 = if v0 { v1 } else { v2 };
    ```
  }];

  let arguments = (ins I1:$condition,
                       EmitRust_SelectValueType:$trueValue,
                       EmitRust_SelectValueType:$falseValue);
  let results = (outs EmitRust_SelectValueType:$result);

  let assemblyFormat = [{
    $condition `,` $trueValue `,` $falseValue attr-dict `:` type($result)
  }];
}

//===----------------------------------------------------------------------===//
// IfOp
//===----------------------------------------------------------------------===//

def EmitRust_IfOp : EmitRust_Op<"if", [
    SingleBlockImplicitTerminator<"emitrust::YieldOp">, NoRegionArguments]> {
  let summary = "If-then-else operation";
  let description = [{
    The `emitrust.if` operation represents an if-then-else construct for
    conditionally executing two regions of code. The operand is a boolean
    value. The "then" region has exactly one block; the "else" region may be
    absent. Blocks are terminated with `emitrust.yield`, which can be left
    out and inserted implicitly. This operation produces no results.

    Example:

    ```mlir
    emitrust.if %cond {
      emitrust.call_opaque "then_branch"() : () -> ()
    } else {
      emitrust.call_opaque "else_branch"() : () -> ()
    }
    ```
  }];

  let arguments = (ins I1:$condition);
  let results = (outs);
  let regions = (region SizedRegion<1>:$thenRegion, AnyRegion:$elseRegion);

  let assemblyFormat = "$condition $thenRegion (`else` $elseRegion^)? attr-dict";
}

//===----------------------------------------------------------------------===//
// ForOp
//===----------------------------------------------------------------------===//

def EmitRust_ForOp : EmitRust_Op<"for", [
    AllTypesMatch<["lowerBound", "upperBound", "step"]>,
    SingleBlockImplicitTerminator<"emitrust::YieldOp">]> {
  let summary = "For operation";
  let description = [{
    The `emitrust.for` operation represents a Rust loop over a stepped range:

    ```rust
    for i in (lb..ub).step_by(step) { /* ... */ }
    ```

    The operation takes three SSA operands representing the lower bound,
    upper bound, and step, all of the same integer or index type. The
    `inclusive` unit attribute selects Rust's inclusive range (`lb..=ub`,
    the image of C's ascending `i <= HI` loop); its absence keeps the
    half-open `lb..ub`. Its single
    region has one block whose sole argument is the induction variable, of
    the same type as the bounds. The trailing colon-type is optional in the
    custom syntax and defaults to `index`. The body block is terminated with
    `emitrust.yield`, which may be left out and inserted implicitly. The
    operation produces no results.

    Example:

    ```mlir
    // Index case.
    emitrust.for %i = %lb to %ub step %s {
      emitrust.call_opaque "body"(%i) : (index) -> ()
    }
    // Integer case.
    emitrust.for %j = %lb32 to %ub32 step %s32 : i32 {
    }
    ```
  }];

  let arguments = (ins EmitRust_IntegerOrIndexType:$lowerBound,
                       EmitRust_IntegerOrIndexType:$upperBound,
                       EmitRust_IntegerOrIndexType:$step,
                       UnitAttr:$inclusive);
  let results = (outs);
  let regions = (region SizedRegion<1>:$region);

  let extraClassDeclaration = [{
    /// Returns the induction variable: the single argument of the body block.
    ::mlir::Value getInductionVar() { return getBody()->getArgument(0); }
  }];

  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// LoopOp
//===----------------------------------------------------------------------===//

def EmitRust_LoopOp : EmitRust_Op<"loop", [
    SingleBlockImplicitTerminator<"emitrust::YieldOp">, NoRegionArguments]> {
  let summary = "Infinite loop operation";
  let description = [{
    The `emitrust.loop` operation represents a Rust `loop` expression used
    as a statement: an unconditional loop that only terminates through an
    `emitrust.break` in its body (or by diverging). The operation takes no
    operands and produces no results. Its single region has one block
    without arguments, terminated with `emitrust.yield`, which may be left
    out and inserted implicitly.

    Example:

    ```mlir
    emitrust.loop {
      emitrust.if %done {
        emitrust.break
      }
    }
    ```
    ```rust
    // Code emitted for the operation above.
    loop {
        if v0 {
            break;
        }
    }
    ```
  }];

  let results = (outs);
  let regions = (region SizedRegion<1>:$region);

  let assemblyFormat = "$region attr-dict";
}

//===----------------------------------------------------------------------===//
// WhileOp
//===----------------------------------------------------------------------===//

def EmitRust_WhileOp : EmitRust_Op<"while", [NoRegionArguments]> {
  let summary = "While loop operation with a foldable condition region";
  let description = [{
    The `emitrust.while` operation represents a Rust `while` expression used
    as a statement (FR-61c). Its `condition` region holds a PURE chain of
    single-use ops ending in `emitrust.condition`; the Rust emitter folds
    the whole chain into the while head through the FR-61d single-use
    inlining, so the region must render statement-free (the conversion that
    creates the op guarantees it; the emitter rejects a non-foldable
    condition with a located error rather than emitting wrong code). The
    `body` region renders as the loop body; loop-carried values stay
    `let mut` bindings above the op, reassigned at the body's end, exactly
    like the `emitrust.loop` lowering -- the win over `emitrust.loop` is
    eliminating the body-if / exit-copy / tail-break scaffolding of
    SSA-destruction, not the value plumbing.

    Example:

    ```mlir
    emitrust.while {
      %c = emitrust.cmp lt, %i, %n : (i32, i32) -> i1
      emitrust.condition %c
    } do {
      emitrust.assign %i = %next : i32
    }
    ```
    ```rust
    // Code emitted for the operation above.
    while i < n {
        i = next;
    }
    ```
  }];

  let results = (outs);
  let regions = (region SizedRegion<1>:$condition, SizedRegion<1>:$body);

  let assemblyFormat = "$condition `do` $body attr-dict";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// ConditionOp
//===----------------------------------------------------------------------===//

def EmitRust_ConditionOp : EmitRust_Op<"condition",
    [Pure, Terminator, HasParent<"WhileOp">]> {
  let summary = "Condition terminator of an emitrust.while condition region";
  let description = [{
    The `emitrust.condition` operation terminates the condition region of
    `emitrust.while`, carrying the i1 the while head tests. It emits no
    Rust code itself; the emitter renders its operand (usually an inlined
    FR-61d expression) as the `while <cond>` head in the never-parenthesized
    condition position.
  }];

  let arguments = (ins I1:$condition);
  let assemblyFormat = "$condition attr-dict";
}

//===----------------------------------------------------------------------===//
// SwitchOp
//===----------------------------------------------------------------------===//

def EmitRust_SwitchOp : EmitRust_Op<"switch", [
    SingleBlockImplicitTerminator<"emitrust::YieldOp">, NoRegionArguments]> {
  let summary = "Switch operation emitted as a Rust match";
  let description = [{
    The `emitrust.switch` operation dispatches on an integer or index
    discriminator, emitted as a Rust `match` statement. It carries one case
    region per entry of its `cases` attribute plus a mandatory default
    region; case values must be unique. All regions have a single block
    without arguments, terminated with `emitrust.yield`, which may be left
    out and inserted implicitly. The operation produces no results.

    Each case renders as a literal integer arm (no type suffix) whose
    pattern is the stored 64-bit case value interpreted in the
    discriminator's Rust type, so the literal compares equal at runtime:
    unsigned decimal of the low bits for a `usize` (index) or unsigned
    discriminator, the sign-interpreted value of the discriminator's width
    for a signed one. The default region renders as the `_ =>` arm, making
    the match exhaustive. An `emitrust.break` or `emitrust.continue`
    inside a case is transparent with respect to an enclosing loop, exactly
    as under an `emitrust.if`.

    This op is the C `switch` on integers: literal arms, no bindings, no
    results, and a MANDATORY default arm because an integer scrutinee can
    always miss every case. The closed-enum match with variant patterns,
    payload bindings, and verifier-enforced exhaustiveness (no default
    arm at all) is the separate `emitrust.match`.

    Example:

    ```mlir
    emitrust.switch %d : i32
    case 0 {
      emitrust.call_opaque "zero"() : () -> ()
    }
    case 4 {
      emitrust.call_opaque "four"() : () -> ()
    }
    default {
    }
    ```
    ```rust
    // Code emitted for the operation above.
    match v0 {
        0 => {
            zero();
        }
        4 => {
            four();
        }
        _ => {
        }
    }
    ```
  }];

  let arguments = (ins AnyTypeOf<[AnyInteger, Index]>:$discriminator,
                       DenseI64ArrayAttr:$cases);
  let results = (outs);
  // ODS requires the variadic region to be declared last, so the default
  // region is stored first (mirroring upstream scf.index_switch); the
  // textual assembly nevertheless prints the case regions first.
  let regions = (region SizedRegion<1>:$defaultRegion,
                        VariadicRegion<SizedRegion<1>>:$caseRegions);

  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// MatchOp
//===----------------------------------------------------------------------===//

def EmitRust_MatchOp : EmitRust_Op<"match", [
    SingleBlockImplicitTerminator<"emitrust::YieldOp">]> {
  let summary = "Closed data-enum match operation";
  let description = [{
    The `emitrust.match` operation matches on a value of a closed data
    enum (`!emitrust.data_enum`, defined by a visible
    `emitrust.data_enum_def`), emitted as a Rust `match` with variant
    patterns. It carries one case region PER VARIANT of the definition, in
    declaration order: a closed enum makes the match exhaustive by
    construction, so — in deliberate contrast to `emitrust.switch`, whose
    integer scrutinee forces a mandatory default region — there is no
    default arm at all, and the verifier enforces total coverage. Each
    case region's single block takes one block argument per payload field
    of its variant (types verified against the definition); the emitted
    arm binds the fields to those names, `_`-prefixed when never read, so
    the deny-manifest `unused_variables` lint stays clean.

    The operation has two modes. In STATEMENT mode it produces no result;
    case blocks terminate with the operand-less `emitrust.yield`, which
    may be left out and inserted implicitly, and the match renders as a
    statement. In RESULT mode it produces exactly one result; every case
    block must terminate with an `emitrust.yield` carrying one value of
    the result type, which renders as that arm's tail expression, and the
    whole match renders as an expression: the right-hand side of a `let`,
    or — through the FR-61a tail fold — the bare tail expression of the
    enclosing function. An `emitrust.break` or `emitrust.continue` inside
    a case is transparent with respect to an enclosing loop, exactly as
    under an `emitrust.switch`.

    Example:

    ```mlir
    %r = emitrust.match %m : !emitrust.data_enum<"Msg"> -> i32
    case "Quit" {
      %c = emitrust.constant <0 : i32> : i32
      emitrust.yield %c : i32
    }
    case "Move" (%x : i32, %y : i32) {
      %s = emitrust.add %x, %y : i32
      emitrust.yield %s : i32
    }
    ```
    ```rust
    // Code emitted for the operation above.
    let v1: i32 = match v0 {
        Msg::Quit => {
            0i32
        }
        Msg::Move { x: v2, y: v3 } => {
            v2 + v3
        }
    };
    ```
  }];

  let arguments = (ins EmitRust_DataEnumType:$scrutinee,
                       StrArrayAttr:$variants);
  let results = (outs Optional<AnyType>:$result);
  let regions = (region VariadicRegion<SizedRegion<1>>:$caseRegions);

  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// BreakOp
//===----------------------------------------------------------------------===//

def EmitRust_BreakOp : EmitRust_Op<"break"> {
  let summary = "Loop break operation";
  let description = [{
    The `emitrust.break` operation exits the innermost enclosing
    `emitrust.loop` or `emitrust.for`. It may appear anywhere inside the
    loop body, including nested `emitrust.if` and `emitrust.switch`
    regions, but the search for the enclosing loop stops at function
    boundaries. The operation takes no
    operands, produces no results, and carries no traits so that it is never
    eliminated as dead code.

    Example:

    ```mlir
    emitrust.loop {
      emitrust.break
    }
    ```
    ```rust
    // Code emitted for the operation above.
    loop {
        break;
    }
    ```
  }];

  let assemblyFormat = "attr-dict";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// ContinueOp
//===----------------------------------------------------------------------===//

def EmitRust_ContinueOp : EmitRust_Op<"continue"> {
  let summary = "Loop continue operation";
  let description = [{
    The `emitrust.continue` operation skips to the next iteration of the
    innermost enclosing `emitrust.loop` or `emitrust.for`. It may appear
    anywhere inside the loop body, including nested `emitrust.if` and
    `emitrust.switch` regions, but the search for the enclosing loop stops
    at function boundaries. The
    operation takes no operands, produces no results, and carries no traits
    so that it is never eliminated as dead code.

    Example:

    ```mlir
    emitrust.for %i = %lb to %ub step %s {
      emitrust.continue
    }
    ```
    ```rust
    // Code emitted for the operation above.
    for v3 in (v0..v1).step_by(v2 as usize) {
        continue;
    }
    ```
  }];

  let assemblyFormat = "attr-dict";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// YieldOp
//===----------------------------------------------------------------------===//

def EmitRust_YieldOp : EmitRust_Op<"yield",
    [Pure, Terminator,
     ParentOneOf<["IfOp", "ForOp", "LoopOp", "SwitchOp", "GlobalCellsOp",
                  "WhileOp", "MatchOp"]>]> {
  let summary = "Block termination operation";
  let description = [{
    The `emitrust.yield` operation terminates the regions of `emitrust.if`,
    `emitrust.for`, `emitrust.loop`, `emitrust.switch`,
    `emitrust.global_cells`, and `emitrust.match`. It produces no results.
    In every position except a result-mode `emitrust.match` case it takes
    no operands, emits no Rust code, exists purely to satisfy MLIR's
    structural requirement that blocks end with a terminator, and may be
    omitted in the custom syntax. Inside a case region of an
    `emitrust.match` that produces a result it instead carries exactly one
    operand — the value the case arm yields, verified by the match against
    its result type and rendered as the arm's tail expression.
  }];

  let arguments = (ins Variadic<AnyType>:$results);

  let builders = [OpBuilder<(ins), [{ /* operand-less terminator */ }]>];

  let assemblyFormat = "($results^ `:` type($results))? attr-dict";
  let hasVerifier = 1;
}

#endif // EMITRUST_EMITRUSTOPS
