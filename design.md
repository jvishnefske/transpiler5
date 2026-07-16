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
| emitrust-cc | tools/emitrust-cc | End-to-end driver: import (one or more C files, with -I/-isystem/--extra-arg), pass pipeline, Rust emission, cargo crate layout, optional cargo build |
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
| enum | named reference to a module-level enum_def | the bare enum name |
| fn_ptr | parenthesized parameter list plus optional arrow result; components restricted to emitter scalars, struct, enum, and nested fn_ptr | nullable function pointer, Option of fn; the C null pointer is None |

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
| call_indirect | fn_ptr callee + variadic arguments + at most one result; verifier requires argument and result types equal the callee signature | call through the Option, expect("null function pointer") then the argument list; a let when a result exists, a statement otherwise |
| constant | typed or opaque value attribute, one result | let binding initialized with the constant |
| literal | string attribute, one result | let binding initialized with the verbatim expression |
| let | one init operand, optional mut marker, one result of same type | let or let mut rebinding |
| assign | destination + value, same types, no result | assignment statement; verifier requires destination be a mut let result |
| add, sub, mul, div, rem | two operands, one result, all same type | infix binary expression |
| cmp | predicate enum (eq, ne, lt, le, gt, ge) + two same-typed operands, i1 result; enum and fn_ptr operands allow eq and ne only (PartialEq but no ordering) | infix comparison |
| cast | one operand, one result; enum and bool result types are rejected (no Rust as-cast produces them) | as-cast expression |
| select | i1 condition + two same-typed value operands, one result (lvalues excluded) | let binding initialized with an if-else expression |
| if | i1 condition + then region + optional else region, no results | if / if-else statement |
| for | lower bound, upper bound, step + single-region body with induction argument | for loop over a stepped range |
| loop | single-region body, no operands or results | infinite loop statement |
| break, continue | no operands or results; must sit inside a loop or for | break / continue statements |
| yield | terminator of if/for/loop regions, no operands | nothing (structural) |
| switch | index argument + default region stored first + variadic case regions with i64 case values (scf.index_switch-style assembly) | match statement with literal arms and a trailing underscore default arm |
| struct_def | module-level symbol with field names and types | derive Clone, Copy, Default struct item |
| enum_def | module-level symbol with variant names and i64 discriminant values | repr(i32) derive Clone, Copy, PartialEq, Default enum item; the first variant carries the default attribute |
| variable | optional scalar init attribute, one lvalue result | mutable local declaration with explicit default |
| member | struct lvalue + field name, lvalue result | place suffixed with dot-field |
| subscript | array or slice lvalue + integer index, lvalue result | place indexed with the value cast to usize |
| deref | ref or mut_ref operand, lvalue result | parenthesized pointer dereference place |
| load | lvalue operand, value result | let binding initialized from the place expression |
| addr_of | lvalue operand, optional mut marker, ref/mut_ref result | let binding of a shared or mutable borrow of the place |
| slice_of | array or slice lvalue + integer index, optional mut marker, ref/mut_ref-of-slice result | let binding of a borrow of the place's tail range from the index (cast to usize) |
| impl | module-level, struct-name attribute + single-region body holding only funcs whose first argument is mut_ref of the named struct | inherent impl block; contained functions render with the receiver named self and the signature spelled &mut self |
| method_call | struct lvalue receiver + method-name attribute + variadic value arguments, at most one result; the method name is not cross-checked (member/struct precedent) | place.method(args) — auto-ref scopes the &mut borrow to the call expression; a let with a result, a statement otherwise |

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
  arithmetic, comparisons (float != maps arith.cmpf une onto Rust's !=,
  which is IEEE unordered-or-unequal exactly; ordered-and-unequal `one` is
  illegal because nothing in the pipeline produces it and Rust has no exact
  rendering), casts (index_castui zero-extends by hopping through the
  unsigned type of the source width, matching lift-cf-to-scf's
  zero-extended switch case values), select, func constructs, and scf if,
  while, for, and index_switch lower to EmitRust; unsigned operations fail
  loudly; the composite convert-to-emitrust pass feeds emitrust-translate.
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
  with located diagnostics, never wrong output; this includes Rust-keyword
  identifiers (struct, field, enum, function, and enumerator names), the
  reserved function name c_main, and printf format strings containing NUL
  or non-printable/non-ASCII bytes. (test/Import/C/goto.c,
  switch-invalid.c, enums-invalid.c, keywords-invalid.c, printf-invalid.c,
  unsigned.c, union.c, varargs-def.c)
- [x] FR-20 Driver stages: emitrust-cc emits pipeline MLIR, Rust source
  with the c_main wrapper, and a buildable cargo crate layout, and rejects
  out-of-subset input with a nonzero exit. (test/Driver/emit-rust.c,
  emit-crate.c, emit-mlir.c, reject.c)
- [x] FR-21 Differential execution: for each end-to-end program the
  clang-built binary and the cargo release build produce identical stdout;
  release mode is load-bearing because debug Rust panics on overflow where
  C wraps, and the programs avoid undefined behavior by construction.
  (test/EndToEnd/loops.c, structs.c, float.c, switch-enum.c, gated on cargo
  in the shell)
- [x] FR-22 Switch statements: C switch (including fall-through, shared
  case labels, nested switches, and negative or 64-bit case values —
  arm values render interpreted in the scrutinee's type, so the
  zero-extended usize scrutinee matches its zero-extended arms) imports as
  cf.switch, lift-cf-to-scf recovers scf.index_switch, the conversion
  lowers it to emitrust.switch, and the emitter renders a Rust match with
  literal arms and an underscore default arm; Duff's device and GNU case
  ranges are rejected with located diagnostics. (test/Import/C/switch.c,
  switch-invalid.c, test/Conversion/SCFToEmitRust/index-switch.mlir,
  test/Target/Rust/match.mlir, test/EndToEnd/switch-enum.c,
  test/EndToEnd/switch-general.c)
- [x] FR-23 C enums: complete named enums become emitrust.enum_def and
  render as repr(i32) Rust enums deriving Clone, Copy, PartialEq, Default;
  enumerator constants render as Name::Variant; enum equality compares the
  enum type directly while relational comparisons and integer arithmetic go
  through explicit i32 discriminant casts; int-to-enum conversions are
  rejected with located diagnostics. (test/Import/C/enums.c,
  enums-invalid.c, test/Target/Rust/match.mlir,
  test/EndToEnd/switch-enum.c)
- [x] FR-24 c-testsuite conformance ledger: the c-testsuite single-exec
  corpus (third_party/c-testsuite submodule) runs differentially through
  run_c_testsuite.py — every transpiled test's cargo build must produce
  byte-identical stdout and exit status 0 against the recorded expected
  output; any mismatch is a fatal MISCOMPILE unless explicitly quarantined
  in known-miscompiles.txt, and the expected-pass.txt manifest ratchets in
  both directions (regressions and unrecorded passes both fail). Current
  ledger: 220 total, 85 transpiled, 85 passed, 0 miscompiled,
  135 unsupported (the remaining tests need unions, pointer-to-pointer or
  void* casts, pointer globals, aggregate initializers, string literals,
  or system-header contents outside the C subset). (test/CTestSuite/)
- [x] FR-25 Generality beyond test vectors: an adversarial audit plus
  differential stress run over shapes absent from the original tests
  (negative/sparse/INT_MAX-adjacent case labels, nested switch, default
  mid-body, deep fall-through, 64-bit switch, mixed-sign enum
  discriminants, float !=, early-return chains folding to arith.select,
  Rust-keyword identifiers, printf byte-content edge cases) found and
  fixed four silent-miscompile or invalid-Rust classes; all shapes are now
  either differentially verified or rejected with located diagnostics.
  Non-finite %f values are routed through the once-per-module
  __emitrust_fmt_f64 helper so NaN prints with C's "nan"/"-nan" spelling
  (Rust's {:.6} would print "NaN"); infinities are differentially verified.
  Remaining documented divergence: only the *sign* of a NaN produced by
  0.0/0.0, which C leaves unspecified (Annex F) and which observably
  differs between gcc and clang on identical source, so it cannot be
  diffed against any single reference.
  (test/EndToEnd/switch-general.c, nonfinite.c,
  test/Import/C/keywords-invalid.c, printf-invalid.c, printf.c,
  test/Conversion/ArithToEmitRust/arith-to-emitrust.mlir,
  unsigned-invalid.mlir, test/Target/Rust/match.mlir)
- [x] FR-26 Multiple translation units: emitrust-cc accepts several C files
  and merges them into one flat crate. External functions and globals are
  unified across translation units (a prototype in one file resolves to a
  definition in another; a second external definition of the same symbol,
  including two mains, is a located diagnostic); file-`static` (internal
  linkage) functions and globals are mangled per translation unit so equal
  spellings in different files stay distinct; an extern object or function
  referenced but defined in no unit is rejected with a located diagnostic
  (variadic prototypes such as printf remain skipped); a struct or enum
  redefined identically via a shared header is deduplicated, while the same
  name with a different shape is a diagnostic. The merged crate is
  differentially identical to the clang-linked native binary and contains no
  unsafe. (test/Import/C/multi-tu.c, multi-tu-undefined-extern.c,
  test/EndToEnd/multi-tu.c)
- [x] FR-27 Include paths: emitrust-cc and emitrust-import-c accept -I,
  -isystem, and --extra-arg and pass them to clang, and clang's builtin
  resource directory is wired in at configure time (with an
  EMITRUST_RESOURCE_DIR environment override), so sources that #include a
  project-local header resolve. (test/Import/C/include-path.c with
  Inputs/helper.h)
- [x] FR-28 Pointer decomposition and slice parameters: pointers never
  enter MLIR. A union-find pre-pass (PointerRegionAnalysis) resolves each
  local pointer to one base object plus an i64 element cursor in a
  promotable memref cell; dereference and subscript become
  emitrust.subscript(base, cursor), pointer arithmetic becomes cursor
  arithmetic, and same-object difference/comparison become plain i64 arith
  ops. Pointer parameters classify per definition: deref/arrow-only
  parameters stay !emitrust.mut_ref<T>, while subscripted, walked,
  compared, reassigned, or passed-on parameters become
  !emitrust.mut_ref<!emitrust.slice<T>> (rendered &mut [T]) dereferenced
  once into the region base of the same decomposition, so array arguments
  decay to emitrust.slice_of borrows and no borrow is held across
  statements. Call sites materialize all value arguments before any
  borrow, and two borrows of one region base are a located rejection, as
  are multi-base rebinding, escaping (&p), NULL constants, string-literal
  and global targets, and cross-TU calls imported before a definition
  refines a parameter to a slice. Zero unsafe in the emitted crates.
  (test/Import/C/pointers-local.c, pointers-local-invalid.c,
  pointers-param-slice.c, pointers-param-invalid.c,
  test/Target/Rust/slice.mlir, test/EndToEnd/pointers-local.c,
  pointer-params.c)
- [x] FR-29 Function pointers: non-variadic C function pointers import as
  the nullable !emitrust.fn_ptr type rendering Option of fn (Copy,
  PartialEq, Default None, so it is a legal struct field, global,
  local, parameter, and return type with NULL mapping to None and no
  sentinel or unsafe). Function references (plain decay and address-of)
  become opaque Some(name) constants after the referenced function's
  imported signature is checked against the pointer type; indirect calls
  (fp(...), (*fp)(...), v.op(...), and calls through returned pointers,
  including 00124's nested fn-ptr-returning shape) become
  emitrust.call_indirect with direct-call argument/result checking and a
  rendering that refines the null-call UB into a deterministic panic;
  truth tests and ==/!= against NULL or another pointer become
  emitrust.cmp eq/ne against a None constant. Function pointers are
  ordinary values that bypass the Phase-1a pointer decomposition.
  Located rejections: variadic targets, signature mismatches (including
  prototype-less K&R pointers bound to functions with parameters),
  argument-carrying calls through prototype-less pointers, fn_ptr
  component types outside the supported set (e.g. data-pointer
  parameters), and arrays of function pointers. The differential test is
  byte-identical to clang and the emitted crate contains no unsafe.
  (test/Import/C/fn-pointers.c, fn-pointers-invalid.c,
  test/Target/Rust/fn-pointers.mlir, test/Dialect/EmitRust/types.mlir,
  ops.mlir, invalid.mlir, test/EndToEnd/fn-pointers.c; c-testsuite
  00087, 00088, 00124)
- [x] FR-30 Owner-struct actors: each qualifying pointer ownership region
  becomes a Rust struct owning its array, and the C functions whose
  pointers resolve into that region become &mut self methods on it — the
  actor is an ownership boundary, not a thread. A pure-AST interprocedural
  pre-pass (Pass A, planOwners, run per TU before any IR is built) unifies
  each pointer call argument's root object with the callee definition's
  parameter in a program-wide union-find; a class is promoted only when
  ALL of: single non-escaping storage base; the base is a local array of
  1..32 elements (the struct_def derive(Default) MVP limit) whose element
  type matches every unified parameter's pointee; the region crosses at
  least one function boundary; and every unified function is defined in
  the TU, returns a plain value, resolves all of its own data-pointer
  parameters into the same class, and has all call sites visible (external
  linkage qualifies only in a whole-program single-TU import). The base
  imports as emitrust.struct_def @Owner_<fn>_<base> ["data"] plus a
  struct-typed variable whose accesses rewrite to member("data"); each
  method imports as a func.func carrying emitrust.method_of, taking
  mut_ref<struct> plus one i64 element index per pointer parameter
  (decomposed against deref(arg0) -> member("data") with the FR-28 cursor
  machinery); call sites lower pointer arguments to i64 cursors and borrow
  the owner for one emitrust.method_call-tagged func.call.
  convert-func-to-emitrust materializes the emitrust.impl surface and
  rewrites tagged calls into emitrust.method_call place expressions
  (erasing the borrow — no materialized borrow survives), including
  sibling method-to-method calls through the dereferenced receiver
  ((*self).m(...)). Every unmet condition (two bases, oversized arrays,
  two-region functions, escaping regions, undefined callees) is a SILENT
  fallback to the FR-28 slice lowering, never an error; owner-struct
  symbol collisions are located rejections. The phase unlocks no new
  c-testsuite tests by design and the ledger shows zero movement; the
  differential test is byte-identical to clang with zero unsafe.
  (test/Dialect/EmitRust/ops.mlir, invalid.mlir,
  test/Conversion/FuncToEmitRust/impl.mlir, test/Target/Rust/impl.mlir,
  test/Import/C/owners.c, owners-fallback.c, test/EndToEnd/owners.c)

## C99 Support Roadmap

Everything the importer must handle before it can claim full C99 language
support, grouped by area. The same validation policy applies as for the
functional requirements: a box is ticked only when a lit regression test
under ninja check-emitrust exercises the feature end to end (import,
conversion, emission, and — for anything executable — differential
execution). Checked items cite the tests that already validate them.
Items marked "design decision needed" cannot be implemented mechanically;
the Rust mapping must be settled first, within the project's no-unsafe
rule.

### Types

- [x] C99-1 Signed integer types char, short, int, long, long long as
  i8/i16/i32/i64, _Bool as bool, float and double as f32/f64.
  (test/Import/C/scalars.c, test/EndToEnd/float.c)
- [x] C99-2 Unsigned integer types mapping to u8/u16/u32/u64, including
  unsigned literals, wrap-around semantics, and the unsigned arithmetic,
  comparison, division, remainder, and shift operations end to end through
  the conversion layer. Dialect/emission layer: unsigned-typed
  emitrust.add/sub/mul render as the wrapping_add/wrapping_sub/wrapping_mul
  method calls (C wrap-around; Rust infix panics on debug overflow), while
  div/rem/cmp/bitwise/shift keep the infix operators whose uN semantics
  already match C, and getDefaultValueAttr covers unsigned types (0 : uN).
  The conversion layer guards the unsigned-semantics arith operations
  (divui/remui/shrui, cmpi ult/ule/ugt/uge) to unsigned IntegerType only —
  on signless types they stay illegal instead of miscompiling to the signed
  Rust operators. Importer side: unsigned C types map to MLIR unsigned
  ui8/ui16/ui32/ui64 (never signless); unsigned scalars live in
  emitrust.variable places rather than memref cells (mem2reg would
  materialize a signless arith.constant default); unsigned literals become
  emitrust.constant; all unsigned arithmetic, bitwise, shift, comparison,
  truth-test, ++/--, and compound-assignment forms lower to the emitrust
  ops; switch on an unsigned scrutinee reinterprets it to signless i64
  bit-exactly (case labels above i64::MAX included). Unary minus on an
  unsigned operand is C's modular negation (C99 6.2.5p9) and lowers to
  `0 - x` through the unsigned emitrust.sub, which renders as Rust's
  wrapping_sub on every width (u8/u16/u32/u64) — matching C at 0, 1,
  UINT_MAX, and the INT_MIN bit pattern instead of panicking on debug
  overflow.
  (test/Target/Rust/arith.mlir, test/Dialect/EmitRust/ops.mlir,
  test/Conversion/ArithToEmitRust/unsigned-invalid.mlir,
  test/Import/C/unsigned.c, switch-unsigned.c,
  switch-unsigned64.c, test/EndToEnd/unsigned.c)
- [x] C99-3 Integer promotions and the usual arithmetic conversions for
  mixed signed/unsigned and mixed-rank expressions: clang Sema supplies the
  implicit casts, and the importer lowers every integer-to-integer cast
  shape touching an unsigned type to emitrust.cast, whose Rust `as`
  semantics (truncation, zero-extension from unsigned, sign-extension from
  signed, same-width reinterpretation) match C's conversions exactly;
  signless-to-signless casts keep the existing arith ext/trunc fast path.
  Unary '-' interacts correctly with the promotions: on an operand that
  stays unsigned after promotion it lowers to the wrapping `0 - x` (see
  C99-2), while an unsigned char/short operand is promoted to signed int
  first (per C) and takes the signed negation path, converting back on
  any narrowing store.
  (test/Import/C/unsigned.c, test/EndToEnd/unsigned.c)
- [ ] C99-4 Plain char signedness policy, character constants, and
  escape sequences.
- [x] C99-5 Enumerations: enum definitions, enumerator constants in
  expressions and case labels, mapped to real Rust enums rather than bare
  integer constants; enum-to-int conversions (implicit promotions in mixed
  enum/int comparisons and arithmetic, and explicit casts) lower to
  `emitrust.cast` on the mapped destination type — signless i32 for signed
  underlying types, ui32 for unsigned ones, so C's unsigned comparison
  against negative ints is preserved — rendered as safe Rust `as` casts of
  the `#[repr(i32)]` enum; int-to-enum conversions remain rejected by
  design (safe Rust has no fallible discriminant cast in the subset), as
  are comparisons between distinct enum types.
  (test/Import/C/enums.c, enum-int.c, enums-invalid.c,
  test/EndToEnd/switch-enum.c, test/EndToEnd/enum-int.c)
- [x] C99-6 Typedefs of every supported type shape, including typedefs of
  pointers, arrays, and struct types, resolved through canonical types; a
  typedef naming an anonymous struct (`typedef struct { ... } T;`) gives
  the record its typedef name for import, mangling, and cross-TU shape
  dedup, while a bare anonymous struct stays rejected with a located
  diagnostic. (test/Import/C/typedefs.c, structs-anon-typedef.c,
  test/EndToEnd/fn-pointers.c)
- [ ] C99-7 Type qualifiers: const (shared reference or immutable let
  mapping), volatile (likely rejected with a diagnostic by policy),
  restrict (accepted and ignored).
- [ ] C99-8 long double (design decision needed: Rust has no extended
  float; document a double mapping or reject).
- [ ] C99-9 _Complex and _Imaginary (design decision needed: no native
  Rust counterpart; likely a documented rejection).

### Declarations and initializers

- [x] C99-10 Block-scoped declarations anywhere in a block and
  declarations in the for-init clause. (test/Import/C/scalars.c)
- [ ] C99-11 Aggregate initializer lists for arrays and structs,
  including nested and partially explicit initializers with implicit
  zeroing.
- [ ] C99-12 Designated initializers for array indices and struct fields.
- [ ] C99-13 Compound literals in expression position.
- [x] C99-14 File-scope objects: global variables with constant
  initializers, tentative definitions, extern declarations across
  translation units (single-TU first), and static file-scope objects
  (Rust mapping: static items; mutable globals are a design decision —
  no unsafe rules out static mut, pointing at interior-mutability
  wrappers or parameter threading).
  Implemented single-TU via module-level `emitrust.global` with
  `emitrust.global_load`/`emitrust.global_store` access ops. Never-written
  const-qualified globals emit plain `static NAME: T = INIT;` read
  directly; every other global emits a `thread_local!`
  `std::cell::Cell<T>` (all imported types are Copy) accessed with
  `.with(|c| c.get()/c.set(v))` — no `unsafe`, no `static mut`, exact for
  the single-threaded subset. No initializer means the type's default
  (C zero-initialization); scalar initializers are clang
  constant-evaluated; element/field access to global aggregates is
  load-modify-store of the whole value. Rejected with located
  diagnostics: taking a global's address, Rust-keyword names, pointer
  types, aggregate initializer lists (deferred to C99-11),
  `_Thread_local`, extern-only declarations, and block-scope extern.
  (test/Dialect/EmitRust/ops.mlir, invalid.mlir,
  test/Target/Rust/globals.mlir, test/Import/C/globals.c,
  globals-invalid.c, globals-keyword.c, globals-extern-only.c,
  globals-aggregate-init.c, globals-thread-local.c, globals-pointer.c,
  globals-extern-local.c, test/EndToEnd/globals.c)
- [x] C99-15 Static local variables preserving state across calls
  (design decision needed for a no-unsafe mapping).
  Implemented with the same `emitrust.global` machinery: a function-local
  static becomes a module-level global mangled `<function>_<name>`
  (collision with any existing module symbol is rejected), constant
  initializer required (C11 6.7.9p4, clang-enforced), initialized once at
  program start. (test/Import/C/globals.c,
  globals-static-collision.c, test/EndToEnd/globals.c)
- [ ] C99-16 Variable-length arrays and variably modified types (design
  decision needed: no fixed-size Rust counterpart; either a documented
  rejection — VLAs are conditionally supported in later standards — or a
  Vec-backed mapping).
- [ ] C99-17 Flexible array members (design decision needed; likely
  rejection).
- [ ] C99-18 inline functions and the C99 inline linkage rules (semantic
  no-op for the transpiler; accept and ignore).

### Expressions and operators

- [x] C99-19 Arithmetic, comparison, logical and/or/not with
  short-circuit evaluation, assignment, compound assignment, and
  statement-position increment/decrement on signed scalars. Unary '-'
  covers unsigned operands too: it lowers to `0 - x` via the unsigned
  emitrust.sub (Rust wrapping_sub), so negation in expressions,
  assignments, and comparisons matches C's modular semantics on all
  supported widths.
  (test/Import/C/scalars.c, test/Import/C/unsigned.c,
  test/EndToEnd/loops.c, test/EndToEnd/unsigned.c)
- [x] C99-20 Bitwise and, or, xor, complement, and shift operators.
  Dialect/emission layer: emitrust.and/or/xor/shl/shr render the Rust
  `&`/`|`/`^`/`<<`/`>>` operators (complement imports as xor with all-ones),
  with conversions from arith.andi/ori/xori/shli on any integer type and
  signedness-guarded shifts right (shrsi only on signless/signed types,
  where Rust `>>` is arithmetic; shrui only on unsigned types, where it is
  logical). Importer side: signless operands lower to
  arith.andi/ori/xori/shli/shrsi and `~x` to xor with all-ones; unsigned
  operands lower directly to the emitrust bitwise ops; a shift amount of a
  different width than the shifted operand is normalized to the operand's
  type (C promotes the two independently), including in `<<=`/`>>=`.
  (test/Dialect/EmitRust/ops.mlir, invalid.mlir,
  test/Target/Rust/arith.mlir,
  test/Conversion/ArithToEmitRust/arith-to-emitrust.mlir,
  unsigned-invalid.mlir, test/Import/C/bitwise.c, unsigned.c,
  test/EndToEnd/bitwise.c)
- [x] C99-21 The conditional operator in expression position, with
  short-circuit arm evaluation (only the selected arm's side effects run)
  through a result cell and a cf diamond; scalar (integer/float) results
  only, non-scalar operands rejected with a located diagnostic.
  (test/Import/C/conditional.c, conditional-invalid.c,
  test/EndToEnd/value-exprs.c)
- [x] C99-22 The comma operator, in value position (left operand for
  effects only, right operand's value) and in statement position (both for
  effects; a void right operand is allowed there).
  (test/Import/C/comma.c, test/EndToEnd/value-exprs.c)
- [x] C99-23 Increment, decrement, and assignment used as values inside
  larger expressions: assignments store and re-load the assigned place
  (the C value is the post-assignment value), postfix ++/-- yield the
  original value and prefix forms the updated one.
  (test/Import/C/value-position.c, test/EndToEnd/value-exprs.c)
- [x] C99-24 Explicit casts between all supported scalar types, including
  unsigned<->signed, unsigned<->float (Rust `as` saturates float-to-int
  where out-of-range C is undefined — a defined refinement), and the
  pre-existing signed/float pairs.
  (test/Import/C/unsigned.c, scalars.c, test/EndToEnd/unsigned.c, float.c)
- [x] C99-25 sizeof and _Alignof on types and expressions, constant-folded
  at import time from clang's target layout into a ui64 (size_t) constant;
  the operand stays unevaluated, and variable-length-array operands are
  rejected with a located diagnostic.
  (test/Import/C/sizeof.c, sizeof-invalid.c, test/EndToEnd/value-exprs.c)
- [x] C99-26 Pointer arithmetic, pointer subtraction, pointer
  comparisons, and array-to-pointer decay: decomposed into (base object,
  i64 cursor) pairs intra-function and slice parameters
  (&mut [T]) across calls — the safe-Rust mapping is slices plus indices
  rather than raw offsets. Multi-base rebinding, escaping pointers
  (&p, pointer struct fields, pointer globals, pointer returns), NULL
  data pointers, void* casts, and string-literal pointers stay located
  rejections by design. See FR-28. Qualifying cross-function regions
  additionally promote to owner structs with &mut self methods (FR-30).
  (test/Import/C/pointers-local.c,
  pointers-param-slice.c, test/EndToEnd/pointers-local.c,
  pointer-params.c)
- [x] C99-27 Function pointers and calls through them: function pointers
  are ordinary Copy values of !emitrust.fn_ptr type rendered
  Option of fn (NULL is None, no sentinel), legal as locals, globals,
  struct fields, parameters, and results; function references become
  signature-checked Some(name) constants, indirect calls become
  emitrust.call_indirect with a deterministic panic refining the
  null-call UB, and truth tests and equality compare against None.
  Variadic pointers, void*/data-pointer components, arrays of function
  pointers, and argument-carrying calls through prototype-less K&R
  pointers are located rejections.
  (test/Import/C/fn-pointers.c, fn-pointers-invalid.c,
  test/EndToEnd/fn-pointers.c, test/Target/Rust/fn-pointers.mlir)
- [ ] C99-28 String literals as char-array initializers and as pointer
  values, with the C escape set (beyond the current printf-format-only
  support).
- [ ] C99-29 Float literal forms including hexadecimal float constants,
  and __func__.

### Statements

- [x] C99-30 if/else, while, for, break, continue, return.
  (test/Import/C/scalars.c, test/EndToEnd/loops.c)
- [x] C99-31 do-while: the body block is entered unconditionally and the
  condition block branches back or exits, so lift-cf-to-scf recovers a
  loop with a trailing conditional break; break/continue target the exit
  and condition blocks through the existing loop stack.
  (test/Import/C/do-while.c, test/EndToEnd/value-exprs.c)
- [x] C99-32 switch, case, default, including fall-through, shared case
  labels, nested switches, and negative/sparse/64-bit case values, mapped
  through cf.switch and scf.index_switch onto a Rust match; Duff's device
  (case labels of an outer switch nested inside inner non-switch
  statements) and GNU case ranges are rejected with located diagnostics.
  (test/Import/C/switch.c, switch-invalid.c, test/EndToEnd/switch-enum.c,
  test/EndToEnd/switch-general.c)
- [ ] C99-33 goto and labels (design decision needed: only reducible
  patterns can be recovered by lift-cf-to-scf; irreducible control flow
  has no direct safe-Rust mapping and may remain a documented rejection).

### Functions and program structure

- [x] C99-34 Function definitions and prototypes with scalar, struct
  by-value, and pointer parameters; forward declarations; main with
  implicit return. (test/Import/C/structs.c, pointers.c, scalars.c)
- [ ] C99-35 Recursive and mutually recursive functions (expected to work
  today; needs a regression test with differential execution).
- [ ] C99-36 Array parameters with decay semantics, including the C99
  static and qualifier forms inside the brackets.
- [ ] C99-37 Variadic function definitions and va_list (design decision
  needed; Rust has no stable varargs — likely a permanent documented
  rejection, with printf-style call sites special-cased as today).
- [x] C99-38 Multiple translation units: several .c files are imported and
  merged into one flat crate with extern object and function resolution
  across units, and internal (static) linkage kept distinct by per-unit
  symbol mangling (a single flat module needs no pub/non-pub visibility).
  Undefined externs and conflicting external definitions are rejected with
  located diagnostics. (test/EndToEnd/multi-tu.c, test/Import/C/multi-tu.c,
  multi-tu-undefined-extern.c) See FR-26.
- [x] C99-39 Preprocessor-heavy sources: #include of project and system
  headers resolves — clang's builtin resource directory is wired in at
  configure time and -I/-isystem/--extra-arg are passed through, so macros,
  conditional compilation, and variadic macros are handled by clang once
  include paths resolve. Project-local headers (-I) are imported eagerly
  and whole-file validated, and are regression-tested. System-header
  declarations (angle-bracket includes / -isystem, gated solely on clang's
  isInSystemHeader at the declaration's expansion location) are skipped at
  the top level instead of imported, so including a real <stdio.h>
  succeeds even though its contents (glibc's anonymous structs in
  bits/types.h, variadic prototypes, FILE) fall outside the supported
  subset. A main-file use of a skipped declaration is rejected at the use
  site with a located diagnostic naming the symbol ("declared in a system
  header; not part of the supported C subset"); types are still imported
  on demand through mapType, and printf keeps its by-name lowering. The
  skip is per-TU and emits no symbols, so multi-TU mangling and cross-TU
  struct dedup are unaffected. (test/Import/C/include-path.c,
  system-headers.c, system-headers-invalid.c, system-headers-multi-tu.c,
  test/EndToEnd/stdio-include.c) See FR-27.

### Aggregates and memory

- [x] C99-40 Struct definitions, member and pointer-member access,
  by-value struct copies, address-of on locals and aggregates, and 1-D
  fixed-size arrays with integer indexing.
  (test/Import/C/structs.c, arrays.c, pointers.c, test/EndToEnd/structs.c)
- [ ] C99-41 Multi-dimensional arrays and arrays of structs, with full
  place-chain access (the dialect's array and member ops already compose;
  the importer must emit the chains and tests must cover them).
- [ ] C99-42 Nested struct types and struct assignment as a whole.
- [ ] C99-43 Pointers to pointers and pointer members inside structs
  (design decision needed alongside C99-26: reference-typed struct fields
  require Rust lifetimes, which the dialect deliberately does not model;
  candidate mappings are index-based handles or ownership restructuring).
- [ ] C99-44 Unions (design decision needed: safe Rust has no untagged
  unions; candidate mappings are enums where usage is disciplined, or
  documented rejection).
- [ ] C99-45 Bit-fields (design decision needed: mask-and-shift accessor
  synthesis, or documented rejection).
- [ ] C99-46 Dynamic memory: malloc, calloc, realloc, free (design
  decision needed under the no-unsafe rule: Box/Vec-based ownership
  reconstruction works only for disciplined allocation patterns; an
  arena-with-handles mapping is the likely general answer).

### Hosted library surface (beyond the language)

- [ ] C99-47 The full printf format language: %s, %c, %u, %x, %o, %e,
  %g, %p, field width, precision, flags, and length modifiers, mapped
  onto Rust format specifications (the current subset is %d, %ld, %f,
  and %%).
- [ ] C99-48 A curated stdio/stdlib/string/math subset mapped to Rust
  equivalents (putchar, puts, abs, string functions over the C99-28
  representation, math intrinsics onto f64 methods), each function
  individually tested differentially.

Where an item above concludes in a documented rejection (varargs
definitions, irreducible goto, _Complex, and similar), that rejection with
a located diagnostic is itself the specified behavior and gets a negative
regression test, exactly like the current subset boundary (FR-19). The
Non-Goals section below describes what the MVP rejects today; items listed
there move out of it as their roadmap boxes are ticked.

## C99 Feature Coverage (in progress)

Same checkbox discipline as the MVP requirements: tick only when the
referenced regression tests pass under ninja check-emitrust.

- [x] C99-14 File-scope objects: global variables with constant
  initializers, tentative definitions, extern declarations across
  translation units (single-TU first), and static file-scope objects
  (Rust mapping: static items; mutable globals are a design decision —
  no unsafe rules out static mut, pointing at interior-mutability
  wrappers or parameter threading).
  Implemented single-TU via module-level `emitrust.global` with
  `emitrust.global_load`/`emitrust.global_store` access ops. Never-written
  const-qualified globals emit plain `static NAME: T = INIT;` read
  directly; every other global emits a `thread_local!`
  `std::cell::Cell<T>` (all imported types are Copy) accessed with
  `.with(|c| c.get()/c.set(v))` — no `unsafe`, no `static mut`, exact for
  the single-threaded subset. No initializer means the type's default
  (C zero-initialization); scalar initializers are clang
  constant-evaluated; element/field access to global aggregates is
  load-modify-store of the whole value. Rejected with located
  diagnostics: taking a global's address, Rust-keyword names, pointer
  types, aggregate initializer lists (deferred to C99-11),
  `_Thread_local`, extern-only declarations, and block-scope extern.
  (test/Dialect/EmitRust/ops.mlir, invalid.mlir,
  test/Target/Rust/globals.mlir, test/Import/C/globals.c,
  globals-invalid.c, globals-keyword.c, globals-extern-only.c,
  globals-aggregate-init.c, globals-thread-local.c, globals-pointer.c,
  globals-extern-local.c, test/EndToEnd/globals.c)
- [x] C99-15 Static local variables preserving state across calls
  (design decision needed for a no-unsafe mapping).
  Implemented with the same `emitrust.global` machinery: a function-local
  static becomes a module-level global mangled `<function>_<name>`
  (collision with any existing module symbol is rejected), constant
  initializer required (C11 6.7.9p4, clang-enforced), initialized once at
  program start. (test/Import/C/globals.c,
  globals-static-collision.c, test/EndToEnd/globals.c)

## Non-Goals for the MVP

Generics, lifetimes beyond simple references, traits and impls, pattern
matching beyond literal match arms, data-carrying enums (C-like unit-variant
enums are supported), error-handling sugar, and expression trees (every
value is a named let binding; no inlining of subexpressions). On the C side
the importer rejects, with located diagnostics: goto, unions, bitfields,
int-to-enum conversions, pointer-to-pointer values, pointer struct fields
and pointer globals, NULL data pointers, void* casts, malloc and friends
(pointer arithmetic, pointer locals, and pointer/array parameters are now
supported through the FR-28 decomposition),
multi-dimensional arrays, aggregate initializers, sizeof/_Alignof of
variable-length-array/incomplete/function operands, conditional operators
with non-scalar results, variadic definitions, and string literals
outside printf. These are natural follow-ons; the emitter's
statement-per-op model is chosen precisely so expression inlining can be
layered in later, as EmitC did.

## Validation

Every requirement above is validated exclusively by regression tests run by
the lit suite; no requirement is checked off based on manual inspection.
The suite must pass warning-free in the pinned dev shell before a
requirement's box may be ticked. CI (.github/workflows/ci.yml) enforces the
same gate on every push and pull request: the full check-emitrust suite in
the pinned Nix dev shell, plus an explicit c-testsuite conformance step in
which every test listed in test/CTestSuite/expected-pass.txt is required to
pass differentially and any MISCOMPILE or ratchet violation fails the run.
