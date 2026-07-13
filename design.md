# EmitRust: An MLIR Dialect for Emitting Rust Source Code

## Purpose

EmitRust is an out-of-tree MLIR dialect modeled on the upstream EmitC dialect.
Where EmitC models C/C++ constructs so that `mlir-translate --mlir-to-cpp` can
emit readable C++, EmitRust models Rust constructs so that
`emitrust-translate --mlir-to-rust` can emit readable, idiomatic-enough Rust.
The dialect is the *last mile* of a lowering pipeline: other dialects are
converted into EmitRust, and the Rust emitter performs a direct, local,
syntax-directed translation with no cleverness.

## Design Principles

- Mirror EmitC's architecture and naming so MLIR developers feel at home:
  dialect + types + attributes defined declaratively in TableGen, a
  translation library under a Target directory, an opt tool, a translate
  tool, and lit/FileCheck regression tests.
- Immutable by default: SSA values translate to immutable Rust let bindings.
  Mutation is opt-in and explicit via a `mut` marker on the let op, enforced
  by the assign op's verifier (Rust's own mutability discipline, mirrored in
  the IR).
- Functional core / imperative shell: the emitter is a pure function from IR
  to text over an output stream; all I/O and CLI concerns live in the
  translate tool's shell.
- Fail loudly and early: every construct the emitter cannot represent in Rust
  produces a diagnostic tied to a source location and a failed translation,
  never silently wrong output. All fallible APIs return LogicalResult or
  FailureOr.
- Verifiers enforce invariants in the type system of the IR itself (e.g. the
  assign op statically requires its destination to be a mutable let binding).

## Architecture

| Component | Location | Responsibility |
|---|---|---|
| Dialect definitions | include/EmitRust (TableGen + C++ headers) | Dialect, type, attribute, enum, and op declarations |
| Dialect implementation | lib/EmitRust | Op verifiers, custom parsers/printers, type/attr implementations |
| Conversion passes | lib/Conversion, include/EmitRust/Conversion | Lowering ub/arith/func/scf into EmitRust; composite convert-to-emitrust pass; optional PDLL showcase patterns |
| C importer | lib/ImportC, include/EmitRust/ImportC.h | clang-LibTooling translation of the C11 subset into hybrid core+EmitRust MLIR |
| Rust emitter | lib/Target/Rust | Translation from EmitRust IR to Rust source text, translation registration |
| emitrust-opt | tools/emitrust-opt | Standard opt tool with the dialect and conversion passes registered |
| emitrust-translate | tools/emitrust-translate | mlir-translate-style tool exposing --mlir-to-rust |
| emitrust-import-c | tools/emitrust-import-c | CLI shell over the importer library, printing the imported module |
| emitrust-cc | tools/emitrust-cc | End-to-end driver: import, pass pipeline, Rust emission, cargo crate layout, optional cargo build |
| Regression tests | test | lit + FileCheck suites for round-trip, diagnostics, emission, conversion, import, driver, and differential execution |

### Pipeline

The end-to-end flow is: parse C with clang LibTooling into a hybrid module
(core cf/arith/memref ops for scalars and control flow, EmitRust
lvalue/aggregate ops for structs, arrays, and pointers), then run upstream
mem2reg and canonicalize to promote scalar cells, lift-cf-to-scf and
canonicalize to recover structured control flow, then convert-to-emitrust to
lower the remaining core dialects, and finally the Rust emitter. The
importer deliberately emits block-based cf so the upstream passes do the
heavy lifting; EmitRust aggregate ops are opaque to those passes and are
never promoted. C main is imported as c_main; crate emission adds a wrapper
main that exits with its result.

Build system: CMake standalone out-of-tree MLIR project (find_package MLIR),
built with Ninja inside the Nix dev shell pinned to LLVM/MLIR 21.

## Dialect Contract (MVP surface)

Dialect namespace: emitrust. C++ namespace: mlir::emitrust.

### Types

| Type | Syntax sketch | Rust rendering |
|---|---|---|
| opaque | opaque type carrying a literal type string | the string verbatim, e.g. String, Vec of i32 |
| ref | ref type with one pointee type parameter | shared reference, ampersand T |
| mut_ref | mut_ref type with one pointee type parameter | mutable reference, ampersand mut T |
| lvalue | assignable place holding a value type | never rendered as a type; places render as expressions |
| array | one-dimensional, size and element type | fixed-size array type, bracket T semicolon N |
| struct | named reference to a module-level struct_def | the bare struct name |

Builtin types accepted by the emitter: i1 renders as bool; signless and
signed 8/16/32/64-bit integers render as i8/i16/i32/i64; unsigned 8/16/32/64
render as u8/u16/u32/u64; index renders as usize; f32 and f64 render as f32
and f64. Any other type is a translation error with a located diagnostic.

### Attributes

| Attribute | Purpose |
|---|---|
| opaque | literal Rust expression string usable as a constant initializer |

### Operations

| Op | Shape | Rust rendering |
|---|---|---|
| use | module-level, string path attribute | use declaration |
| verbatim | statement-level, string attribute | raw text line |
| func | symbol + function type + single-region body, at most one result | fn item with typed parameters and return type |
| return | terminator, optional single operand | return statement |
| call_opaque | string callee + variadic operands + variadic results | function call; zero results a statement, one result a let, many results a tuple-destructuring let |
| constant | typed or opaque value attribute, one result | let binding initialized with the constant |
| literal | string attribute, one result | let binding initialized with the verbatim expression |
| let | one init operand, optional mut marker, one result of same type | let or let mut rebinding |
| assign | destination + value, same types, no result | assignment statement; verifier requires destination be a mut let result |
| add, sub, mul, div, rem | two operands, one result, all same type | infix binary expression |
| cmp | predicate enum (eq, ne, lt, le, gt, ge) + two same-typed operands, i1 result | infix comparison |
| cast | one operand, one result | as-cast expression |
| if | i1 condition + then region + optional else region, no results | if / if-else statement |
| for | lower bound, upper bound, step + single-region body with induction argument | for loop over a stepped range |
| loop | single-region body, no operands or results | infinite loop statement |
| break, continue | no operands or results; must sit inside a loop or for | break / continue statements |
| yield | terminator of if/for/loop regions, no operands | nothing (structural) |
| struct_def | module-level symbol with field names and types | derive Clone, Copy, Default struct item |
| variable | optional scalar init attribute, one lvalue result | mutable local declaration with explicit default |
| member | struct lvalue + field name, lvalue result | place suffixed with dot-field |
| subscript | array lvalue + integer index, lvalue result | place indexed with the value cast to usize |
| deref | ref or mut_ref operand, lvalue result | parenthesized pointer dereference place |
| load | lvalue operand, value result | let binding initialized from the place expression |
| addr_of | lvalue operand, optional mut marker, ref/mut_ref result | let binding of a shared or mutable borrow of the place |

The assign operation additionally accepts any lvalue-typed destination, and
call_opaque optionally carries an args attribute whose entries are either
operand indices or literal attributes (notably quoted strings), which is how
imported printf calls become Rust print macro invocations.

## MVP Functional Requirements

Check a box only when the referenced regression test passes under
ninja check-emitrust in the pinned dev shell. Traceability: each requirement
lists the lit test file(s) that validate it.

- [x] FR-1 Dialect registration and round-trip: emitrust-opt parses and
  re-prints every MVP op with no loss. (test/Dialect/EmitRust/ops.mlir)
- [x] FR-2 Types parse and print: opaque, ref, mut_ref, including nesting.
  (test/Dialect/EmitRust/types.mlir)
- [x] FR-3 Op verifiers reject malformed IR with precise diagnostics:
  assign to a non-let value, assign to a non-mut let, func with more than
  one result, cmp/binary type mismatches, empty opaque type string.
  (test/Dialect/EmitRust/invalid.mlir)
- [x] FR-4 Function emission: signature with parameter and return types,
  body, return statement. (test/Target/Rust/func.mlir)
- [x] FR-5 Expression emission: constant, literal, call_opaque,
  add/sub/mul/div/rem, cmp, cast, with correct type rendering of all
  supported builtin types. (test/Target/Rust/arith.mlir,
  test/Target/Rust/types.mlir)
- [x] FR-6 Mutability discipline: let renders as immutable let, mut let as
  let mut, assign as plain assignment. (test/Target/Rust/statements.mlir)
- [x] FR-7 Control-flow emission: if, if-else, for with stepped range.
  (test/Target/Rust/control-flow.mlir)
- [x] FR-8 Module-level emission: use declarations and verbatim text.
  (test/Target/Rust/module.mlir)
- [x] FR-9 Unsupported constructs fail translation with a located
  diagnostic, not silent bad output. (test/Target/Rust/errors.mlir)
- [x] FR-10 Tooling: emitrust-opt and emitrust-translate build and expose
  the dialect and the mlir-to-rust translation. (all tests exercise both)
- [x] FR-11 One-command regression suite: ninja check-emitrust runs the
  full lit suite inside the Nix dev shell. (test/CMakeLists.txt wiring)
- [x] FR-12 Loop dialect surface: loop, break, and continue ops round-trip,
  reject placement outside a loop, and survive canonicalization.
  (test/Dialect/EmitRust/ops.mlir, invalid.mlir, canonicalize.mlir)
- [x] FR-13 Loop emission: loop bodies with conditional break and continue
  render as Rust loop statements. (test/Target/Rust/loop.mlir)
- [x] FR-14 Memory dialect surface: lvalue, array, and struct types and the
  struct_def, variable, member, subscript, deref, load, and addr_of ops
  round-trip; verifiers reject lvalue function parameters and results,
  mismatched places, and malformed struct definitions.
  (test/Dialect/EmitRust/ops.mlir, types.mlir, invalid.mlir)
- [x] FR-15 Memory emission: struct definitions, variable defaults, field
  and array reads and writes, dereferences, borrows, and printf-style
  call_opaque args render as Rust. (test/Target/Rust/memory.mlir,
  test/Target/Rust/errors.mlir for the cast-to-bool boundary)
- [x] FR-16 Conversion passes: ub.poison, arith constants, signed
  arithmetic, comparisons, and casts, func constructs, and scf if, while,
  for, and index_switch lower to EmitRust; unsigned operations fail loudly;
  the composite convert-to-emitrust pass feeds emitrust-translate.
  (test/Conversion/UBToEmitRust/poison.mlir,
  test/Conversion/ArithToEmitRust/arith-to-emitrust.mlir,
  test/Conversion/ArithToEmitRust/unsigned-invalid.mlir,
  test/Conversion/FuncToEmitRust/func-to-emitrust.mlir,
  test/Conversion/SCFToEmitRust/if.mlir, while.mlir, for.mlir,
  index-switch.mlir, test/Conversion/pipeline.mlir)
- [x] FR-17 C importer, scalars and control flow: scalar locals become
  rank-0 memref cells with explicit cf branches, and the mem2reg plus
  lift-cf-to-scf pipeline recovers alloca-free structured control flow.
  (test/Import/C/scalars.c)
- [x] FR-18 C importer, aggregates and pointers: struct definitions, field
  and array accesses, pointer parameters, address-of, by-value struct
  passing, and printf lowering produce the EmitRust place ops.
  (test/Import/C/structs.c, arrays.c, pointers.c, printf.c)
- [x] FR-19 C importer subset boundary: constructs outside the subset fail
  with located diagnostics, never wrong output. (test/Import/C/goto.c,
  switch.c, unsigned.c, union.c, varargs-def.c)
- [x] FR-20 Driver stages: emitrust-cc emits pipeline MLIR, Rust source
  with the c_main wrapper, and a buildable cargo crate layout, and rejects
  out-of-subset input with a nonzero exit. (test/Driver/emit-rust.c,
  emit-crate.c, emit-mlir.c, reject.c)
- [x] FR-21 Differential execution: for each end-to-end program the
  clang-built binary and the cargo release build produce identical stdout;
  release mode is load-bearing because debug Rust panics on overflow where
  C wraps, and the programs avoid undefined behavior by construction.
  (test/EndToEnd/loops.c, structs.c, float.c, gated on cargo in the shell)

## Non-Goals for the MVP

Generics, lifetimes beyond simple references, traits and impls, pattern
matching, enums, error-handling sugar, and expression trees (every value is
a named let binding; no inlining of subexpressions). On the C side the
importer rejects, with located diagnostics: goto, switch, do-while, unions,
bitfields, enums, all unsigned types, globals, pointer arithmetic and
pointer locals, multi-dimensional arrays, aggregate initializers, sizeof,
the conditional operator, comma, bitwise and shift operators, variadic
definitions, and string literals outside printf. These are natural
follow-ons; the emitter's statement-per-op model is chosen precisely so
expression inlining can be layered in later, as EmitC did.

## Validation

Every requirement above is validated exclusively by regression tests run by
the lit suite; no requirement is checked off based on manual inspection.
The suite must pass warning-free in the pinned dev shell before a
requirement's box may be ticked.
