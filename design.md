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

The theory behind each pipeline stage — the data structures, algorithms, and
theorems it relies on, the guarantee each stage hands the next, and the
technique-to-CTS-item fit assessment — is surveyed with citations in
[docs/transformation-theory.md](docs/transformation-theory.md).

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
  literal arms and an underscore default arm; bodies the structured
  lowering cannot shape (non-compound bodies, statements before the first
  label, case labels nested inside inner statements — Duff's device) take
  the CTS-S2 dispatch fallback instead, and GNU case ranges are rejected
  with located diagnostics. (test/Import/C/switch.c, switch-dispatch.c,
  switch-invalid.c, test/Conversion/SCFToEmitRust/index-switch.mlir,
  test/Target/Rust/match.mlir, test/EndToEnd/switch-enum.c,
  test/EndToEnd/switch-general.c, test/EndToEnd/switch-dispatch.c)
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
  ledger: 220 total, 179 transpiled, 179 passed, 0 miscompiled,
  41 unsupported (the remaining tests need unions, pointer-to-pointer or
  void* casts, pointer globals, anonymous structs, wide strings, or
  system-header contents outside the C subset; see the c-testsuite
  checklist below).
  (test/CTestSuite/)
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
  expressions and case labels, mapped to a distinct nominal Rust type per
  enum rather than bare integer constants; enum-to-int conversions
  (implicit promotions in mixed enum/int comparisons and arithmetic, and
  explicit casts) lower to `emitrust.cast` on the mapped destination type
  — signless i32 for signed underlying types, ui32 for unsigned ones, so
  C's unsigned comparison against negative ints is preserved — rendered as
  safe Rust `as` casts of the raw value; comparisons between distinct enum
  types stay rejected. Revised under CTS-S6: the emitted representation is
  a value-preserving open enum (a `#[repr(transparent)]` tuple struct over
  the storage integer with one associated constant per enumerator and a
  Default impl returning the first variant) rather than a fieldless
  `#[repr(i32)]` enum, because C enum objects hold any value of the
  underlying type; int-to-enum conversions are therefore supported and
  value-preserving (see CTS-S6).
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
- [x] C99-11 Aggregate initializer lists for arrays and structs,
  including nested and partially explicit initializers with implicit
  zeroing.
  Implemented at both scopes. Block scope: the variable keeps its
  default-initialized `emitrust.variable` place (C99 zero-fill), then one
  `emitrust.assign` per explicitly initialized element through a
  constant-index `emitrust.subscript` (arrays) or `emitrust.member`
  (struct fields), recursing for nested lists — this uniformly covers
  partial initialization, designators, and non-constant elements, and
  also applies to owner-promoted arrays. File scope: clang's constant
  evaluator produces the complete APValue (designators resolved, holes
  zero-filled from the array filler), converted to a typed ArrayAttr
  element list on `emitrust.global` (nested ArrayAttr for aggregate
  elements); the GlobalOp verifier checks element count against the array
  size / struct_def field count and per-element types. Const arrays stay
  plain `static NAME: [T; N] = [e0, ...];`; mutable aggregate globals
  keep the `thread_local!` Cell path with `[e0, ...]` / `Name { f: e, ...
  }` literals. Accepted trade-off: a partially initialized large global
  renders its full element list (the 32-element `Default` derive cap only
  constrains struct fields of array type, which use literal lists here
  anyway). Rejected with located diagnostics: non-list aggregate
  initializers (whole-struct copies, compound literals — C99-13 stays
  open; string literals on char arrays are supported per C99-28) and
  enum-typed global elements; multi-dimensional arrays are rejected by
  the type mapper before initializer handling.
  (test/Import/C/aggregate-init.c, aggregate-init-invalid.c,
  test/Dialect/EmitRust/ops.mlir, invalid.mlir,
  test/Target/Rust/globals.mlir, test/EndToEnd/aggregate-init.c,
  c-testsuite 00048/00090/00092/00093/00115/00117/00146/00147/00148)
- [x] C99-12 Designated initializers for array indices and struct fields.
  Implemented with C99-11: the importer works on clang's semantic
  initializer-list form, where `[i] =` and `.field =` designators are
  already resolved to positional elements with implicit-value holes, so
  designated, partial, and overwriting-positional cases all reduce to the
  same per-element handling at block scope and the same APValue
  conversion at file scope. (test/Import/C/aggregate-init.c,
  test/EndToEnd/aggregate-init.c)
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
  constant-evaluated; aggregate initializer lists are typed ArrayAttr
  element lists (C99-11); element/field access to global aggregates is
  load-modify-store of the whole value. Rejected with located
  diagnostics: taking a global's address in value position, Rust-keyword
  names, `_Thread_local`, extern-only declarations, and block-scope
  extern. Pointer-typed file-scope variables import through the CTS-P4
  global region model (single global base plus a stored i64 cursor
  global; see the c-testsuite checklist); pointer-typed function-local
  statics stay rejected.
  (test/Dialect/EmitRust/ops.mlir, invalid.mlir,
  test/Target/Rust/globals.mlir, test/Import/C/globals.c,
  globals-invalid.c, globals-keyword.c, globals-extern-only.c,
  aggregate-init.c, globals-thread-local.c, globals-pointer.c,
  globals-pointer-invalid.c,
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
  (&p, pointer struct fields, pointer returns), NULL
  data pointers, void* casts, and string-literal pointers stay located
  rejections by design (pointer globals are now the CTS-P4 global
  region model). See FR-28. Qualifying cross-function regions
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
  support). PARTIAL: `char s[N] = "..."` / `char s[] = "..."` is
  supported for plain/signed char arrays. Block scope lowers to
  per-element byte assigns over the default-zero place — the literal's
  bytes plus the terminating NUL when it fits (C99 6.7.8p14), remaining
  elements keeping the zero fill; file scope folds through the
  C99-11/14 APValue path to a typed i8 ArrayAttr on `emitrust.global`.
  Embedded NULs in the literal are ordinary data. `char *p = "..."`
  pointer bindings are supported as read-only string-literal regions
  (CTS-P1): the pointer is an i64 cursor into an immutable backing byte
  array holding the literal's bytes plus the terminating NUL, with the
  same ASCII policy on the backing bytes. Located rejections: non-ASCII
  bytes (all scopes, keeping the printed contents exact through the
  ASCII-only %s/%c helpers), wide/unsigned-char element types, and
  writes through a literal-bound pointer (the region is read-only).
  (test/Import/C/strings.c, strings-invalid.c,
  aggregate-init-invalid.c, pointers-string-literal.c,
  test/EndToEnd/strings.c, string-cursor.c)
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
  statements) and other non-plain bodies take the CTS-S2 dispatch
  fallback, and GNU case ranges are rejected with located diagnostics.
  (test/Import/C/switch.c, switch-dispatch.c, switch-invalid.c,
  test/EndToEnd/switch-enum.c, test/EndToEnd/switch-general.c,
  test/EndToEnd/switch-dispatch.c)
- [x] C99-33 goto and labels: each label maps to a dedicated block
  (created at first mention, so forward and backward gotos both resolve)
  and a goto is a plain cf.br; lift-cf-to-scf structures the resulting
  CFG, including irreducible shapes (a goto into a loop body lowers
  through the transformCFGToSCF multiplexer) and backward gotos that form
  loops. In a function containing labels, emitrust.variable places are
  hoisted to the entry block so a goto that jumps over a declaration
  cannot leave a later use undominated. Computed goto (GNU `goto *expr`)
  is rejected with a located diagnostic.
  (test/Import/C/goto.c, goto-invalid.c, test/EndToEnd/goto.c)

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
- [x] C99-41 Multi-dimensional arrays and arrays of structs, with full
  place-chain access (the dialect's array and member ops already compose;
  the importer must emit the chains and tests must cover them).
  `!emitrust.array` elements nest (emitted `[[T; N]; M]`), `mapType`
  recurses, nested initializer lists (file-scope with designators and
  zero fill, and block-scope) recurse level by level, `a[i][j]` emits one
  `emitrust.subscript` per dimension, subscript chains compose with
  `emitrust.member` on arrays of structs, and the pointer decomposition
  reaches into multi-dimensional bases with a flat row-major cursor
  (`&arr[i][j]`, row-pointer subscripts, scalar dereference via div/rem
  peeling). Row-pointer walking arithmetic and slices of rows stay
  located rejections (CTS-P scope). See CTS-S4.
  (test/Dialect/EmitRust/types.mlir, invalid.mlir,
  test/Import/C/arrays-multidim.c, pointers-local-invalid.c,
  test/EndToEnd/arrays-multidim.c)
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
  onto Rust format specifications. PARTIAL — the supported directive
  grammar is `%[flags][width][length]conv` with flags `-` (left align)
  and `0` (zero pad, ignored next to `-` as in C), a decimal width,
  length `l`, and conversions d/i (i32; i64 with `l`), u (u32/u64),
  x/X/o (u32/u64 rendered `{:x}`/`{:X}`/`{:o}`), c, s, f, and %%.
  Width/flags map 1:1 onto Rust specs (`%5d`→`{:5}`, `%-5d`→`{:<5}`,
  `%05d`→`{:05}`, `%04X`→`{:04X}`; Rust's zero pad is sign-aware like
  C's). u/x/X/o `as`-cast the argument to the directive's unsigned
  type, so a negative signed argument prints its two's-complement bit
  pattern exactly like C (`%x` of -1 is ffffffff); an integer argument
  of a different width is `as`-cast likewise, truncating to the low
  bits exactly like C's varargs read on x86-64 (`%d` of a size_t).
  %c routes the int-promoted argument through the on-demand
  `__emitrust_fmt_c` helper; %s accepts exactly two shapes — a string
  literal (lowered to an `emitrust.literal` `&'static str`; embedded
  NUL and non-ASCII bytes rejected) and a char-array lvalue (lowered to
  `emitrust.slice_of` of the whole array through the on-demand
  `__emitrust_cstr` helper, which stops at the first NUL like C); %f is
  unchanged (f64 through `__emitrust_fmt_f64`, `%lf` accepted as its C
  synonym). Located rejections: precision (`%.3s`, `%.2f`), lengths
  `ll`/`h`/`L`/`j`/`z`/`t` (`%llx`, `%10Ld`), conversions outside the
  set (%p, %n, %e, %g), flags/width on %c/%s/%f, and argument type
  mismatches.
  (test/Import/C/printf.c, printf-extended.c, printf-extended-invalid.c,
  strings.c, strings-invalid.c, test/EndToEnd/printf-formats.c,
  test/EndToEnd/strings.c)
- [ ] C99-48 A curated stdio/stdlib/string/math subset mapped to Rust
  equivalents (putchar, puts, abs, string functions over the C99-28
  representation, math intrinsics onto f64 methods), each function
  individually tested differentially. PARTIAL: statement-position
  `puts(s)` and `putchar(c)` are lowered by name when the project
  supplies no definition of its own (a user-defined puts/putchar stays
  an ordinary call): puts to `println!` through the %s machinery (both
  %s shapes), putchar to `print!` of the argument through
  `__emitrust_fmt_c`, matching C's conversion to unsigned char. The
  char helpers are ASCII-only by design: C writes the raw byte where
  Rust would encode code points 128..=255 as two UTF-8 bytes, so
  non-ASCII string data is rejected at import (see C99-28/47) and the
  helpers are exact for everything that gets through. Value uses of the
  puts/putchar result keep located rejections. Math intrinsics: a
  definition-less call to `sin` with its standard double(double)
  prototype lowers to `emitrust.call_opaque "f64::sin"` (both resolve to
  the platform libm, verified differentially); a user-defined `sin`
  stays an ordinary call, and every other math function keeps the
  system-header rejection with a located diagnostic (the natural
  extension point is the `hostedMathCallee` table in ImportC.cpp).
  string.h (CTS-L1): definition-less strcpy/strncpy/strcat/memset/memcpy
  lower by name in statement position, strcmp/strncmp/memcmp and strlen
  in value position, and strchr/strrchr where a printf %s argument or a
  null-pointer comparison consumes the result — each to a one-per-module
  safe Rust helper over `&[i8]`/`&mut [i8]` slices of the argument's
  char region (a char array, a string-literal backing, or a pointer into
  either), so every access is a bounds-checked slice index with no
  unsafe. Comparison helpers compare as unsigned char per C;
  strchr/strrchr return the found index or -1 (C's NULL), which %s
  offsets into the region and null comparisons test directly;
  same-object memcpy borrows the array mutably once and passes both
  cursors (`copy_within`, refining C's undefined overlap). Copy results
  are statement-position only, a copy source sharing the destination's
  object, literal-region destinations, and uncurated functions (strstr,
  strtok, ...) keep located rejections; the helper namespace
  `__emitrust_*` is reserved.
  (test/Import/C/strings.c, strings-invalid.c, strings-hosted.c,
  strings-hosted-invalid.c, math.c, test/EndToEnd/strings.c,
  strings-hosted.c, math-sin.c)

Where an item above concludes in a documented rejection (varargs
definitions, irreducible goto, _Complex, and similar), that rejection with
a located diagnostic is itself the specified behavior and gets a negative
regression test, exactly like the current subset boundary (FR-19). The
Non-Goals section below describes what the MVP rejects today; items listed
there move out of it as their roadmap boxes are ticked.

## C99 Feature Coverage (in progress)

Same checkbox discipline as the MVP requirements: tick only when the
referenced regression tests pass under ninja check-emitrust.

- [x] C99-11 Aggregate initializer lists for arrays and structs,
  including nested and partially explicit initializers with implicit
  zeroing. Block scope: default-initialized place plus one
  `emitrust.assign` per explicit element (constant-index
  `emitrust.subscript` / `emitrust.member`), recursing for nested lists;
  file scope: clang-constant-evaluated APValue converted to a typed
  ArrayAttr element list on `emitrust.global`, verifier-checked against
  the array size / struct_def field count. See the full entry in
  "Declarations and initializers" above.
  (test/Import/C/aggregate-init.c, aggregate-init-invalid.c,
  test/Dialect/EmitRust/ops.mlir, invalid.mlir,
  test/Target/Rust/globals.mlir, test/EndToEnd/aggregate-init.c)
- [x] C99-12 Designated initializers for array indices and struct fields.
  Implemented with C99-11 via clang's semantic initializer-list form
  (designators pre-resolved to positional elements with implicit-value
  holes). (test/Import/C/aggregate-init.c, test/EndToEnd/aggregate-init.c)
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
  constant-evaluated; aggregate initializer lists are typed ArrayAttr
  element lists (C99-11); element/field access to global aggregates is
  load-modify-store of the whole value. Rejected with located
  diagnostics: taking a global's address in value position, Rust-keyword
  names, `_Thread_local`, extern-only declarations, and block-scope
  extern. Pointer-typed file-scope variables import through the CTS-P4
  global region model (single global base plus a stored i64 cursor
  global; see the c-testsuite checklist); pointer-typed function-local
  statics stay rejected.
  (test/Dialect/EmitRust/ops.mlir, invalid.mlir,
  test/Target/Rust/globals.mlir, test/Import/C/globals.c,
  globals-invalid.c, globals-keyword.c, globals-extern-only.c,
  aggregate-init.c, globals-thread-local.c, globals-pointer.c,
  globals-pointer-invalid.c,
  globals-extern-local.c, test/EndToEnd/globals.c)
- [x] C99-15 Static local variables preserving state across calls
  (design decision needed for a no-unsafe mapping).
  Implemented with the same `emitrust.global` machinery: a function-local
  static becomes a module-level global mangled `<function>_<name>`
  (collision with any existing module symbol is rejected), constant
  initializer required (C11 6.7.9p4, clang-enforced), initialized once at
  program start. (test/Import/C/globals.c,
  globals-static-collision.c, test/EndToEnd/globals.c)

## c-testsuite Remaining-Failure Checklist

Ledger as of 2026-07-17: 220 total / 188 passed / 0 miscompiled /
32 unsupported (was 150/70 at commit a091423, when this checklist was
drawn up; the quick wins, the CTS-S7/R5/P4 partials, and
CTS-S1/S2/S4/S6/P1/P8/R1/R4/L1/L2 landed since). Every one of the 32
is a located build-time rejection — never wrong output.
This checklist partitions the original 70 by sole blocker: each item lists the
exact tests it unlocks, so the sum of all items is exactly 70. Same
checkbox discipline as above — tick only when the referenced tests pass
under ninja check-emitrust and the ledger ratchets with zero new
miscompiles. Counts are first-blocker attributions; unlocking one item
can surface a second blocker in the same test (the interaction effect
observed when C99-33 + C99-47 together unlocked 00215).

### Quick wins (7 tests, no design decisions needed)

- [x] CTS-E1 (3) Thread-local closure binder shadows a mutable global
  named `c`: TranslateToRust.cpp hardcodes `.with(|c| c.get())` /
  `.with(|c| c.set(v))`, so a C global literally named `c` makes rustc
  resolve the closure pattern against the thread-local key and fail with
  E0308. Rename the binder to a reserved identifier (e.g. `__tl`).
  These are the only three tests that transpile but fail rustc.
  (00127.c, 00128.c, 00142.c)
  (Done: the binder is `__emitrust_tl`, following the `__emitrust_`
  reserved-prefix convention; the importer rejects a C global spelled
  `__emitrust_tl` like the other reserved helper names. 00127.c, 00128.c,
  00142.c now pass and are in the manifest — ledger 153 passed / 67
  unsupported / 0 miscompiled. Pinned by test/Target/Rust/globals.mlir
  (global named `c`), test/EndToEnd/globals.c (rustc-level), and
  test/Import/C/keywords-invalid.c (reserved-name rejection).)
- [x] CTS-F2 (3) Unreferenced main-file declarations must not demand
  definitions: `extern int x;` or a repeated prototype `int foo(void);`
  that is never referenced currently rejects with "referenced but not
  defined in any translation unit" even though nothing references it.
  Apply the C99-39 referenced-only policy to main-file prototypes and
  extern objects: skip if unreferenced, reject at the use site otherwise.
  (00094.c, 00108.c, 00162.c)
  Done: importFunction/importGlobalVar skip a body-less prototype or
  extern-only object whose redeclaration chain is unreferenced (before
  signature mapping, so unsupported shapes in dead prototypes cannot
  reject either); finalizeProject erases use-free external funcs and
  locates the referenced-but-undefined rejection at the first use site.
  Ledger 150 -> 153, zero miscompiles.
  (test/Import/C/unreferenced-extern-global.c, unreferenced-prototype.c,
  multi-tu-undefined-extern-global.c)
- [x] CTS-S3 (1) Block-scope function prototypes (`int f1(char *);`
  inside a function body): hoist the declaration to module scope and
  continue; currently "unsupported declaration inside a function body".
  (00078.c) Done: `emitStmt` routes a `FunctionDecl` in a `DeclStmt`
  through `importFunction`, the same path as a file-scope prototype
  (external linkage per C11 6.2.2p5); other in-body declarations still
  reject. (test/Import/C/fn-prototypes-local.c; 00078.c in the ledger)

### Pointer model extensions (30 tests, builds on FR-28/C99-26)

- [x] CTS-P1 (7) `char *` bound to string literals: a read-only
  string-region class in PointerRegionAnalysis whose base is the literal
  (`&'static [u8]`/`&'static str`) and whose cursor indexes it; feeds the
  existing %s printf shapes (C99-28/47). Watch embedded-NUL and
  non-ASCII policy already set by C99-28.
  (00025.c, 00026.c, 00058.c, 00112.c, 00137.c, 00138.c, 00173.c)
  Done: PointerRegion carries an optional string-literal base
  (`literalBase`, data on the region, alongside the object bases) plus a
  write-through fact; a literal-based region is a cursor into a read-only
  run backed by an immutable `const`-marked `emitrust.variable` byte
  array (`let lit: [i8; N+1] = [...]`, literal bytes plus the terminating
  NUL so strlen-style walks terminate), created once per literal and
  shared by every pointer of the region. Dereference/subscript read bytes
  via `emitrust.subscript(backing, cursor)`; arithmetic is the usual i64
  cursor arithmetic; any write through the region (`*p = c`, `p[i] = c`,
  `(*p)++`) is a located rejection at the write site (writing a C string
  literal is UB; the region is read-only), as are rebinding across two
  literals and joining a literal with an object. `%s` of a literal-bound
  pointer slices the backing from the cursor through `__emitrust_cstr`
  (C99-28 ASCII policy applies to the backing bytes); a definition-less
  `strlen` lowers by name to the new `__emitrust_strlen` helper over the
  same slice; `"..." == NULL` folds to false (a literal's address is
  never null). Ledger 159 -> 166, zero miscompiles, all seven tests in
  the manifest. (test/Import/C/pointers-string-literal.c; write-through,
  multi-literal, and literal/object-join rejections in
  test/Import/C/pointers-local-invalid.c; rustc-level differential
  test/EndToEnd/string-cursor.c)
- [ ] CTS-P2 (7) Pointer types outside the parameter/local-cursor
  positions FR-28 classifies: pointer returns, pointer struct members
  (C99-43), pointers in casts and mixed expressions. Requires extending
  the region analysis beyond (base, cursor) pairs rooted in one
  function's locals.
  (00019.c, 00049.c, 00095.c, 00140.c, 00150.c, 00208.c, 00214.c)
- [ ] CTS-P3 (5) Pointers assigned non-address values (integer↔pointer
  round-trips, arithmetic results stored back into pointers): needs a
  design decision — either a tagged cursor representation or a
  permanent by-design rejection documented per test. The one carve-out
  is the null pointer constant, which CTS-P8 now models as the None side
  of an Option-of-cursor; every other non-address value (including
  nonzero integers cast to pointers) stays rejected.
  (00039.c, 00103.c, 00144.c, 00163.c, 00187.c)
- [ ] CTS-P4 (4) Pointer-typed global variables: global region bases.
  Hard interaction with the thread_local!+Cell global model (a borrow
  cannot escape `.with`); likely wants globals-as-slices with index
  cursors, or owner-struct promotion to module scope. C99-14 currently
  rejects these by design.
  (00040.c, 00045.c, 00149.c, 00209.c)
  (Partial, 3 of 4: a pointer-typed global decomposes against a single
  *global* region base; its cursor is a stored i64 `emitrust.global`
  under the pointer's C name — a cursor is a borrow-free Copy integer,
  so storing it globally never fights the thread_local!+Cell model, per
  docs/transformation-theory.md section 4. Supported base shapes: a
  global scalar/struct (`int *p = &x;`, degenerate — no runtime state),
  a global array (cursor + the existing staged-copy element access), a
  file-scope compound literal (synthesized `<name>_backing` global,
  00149), and a single constant-size calloc/malloc site promoted to a
  synthesized zero-initialized backing array whose assignment re-zeroes
  it — exact calloc semantics on every execution (00040, whose recursion
  and write-throughs all pass). Unreferenced pointer globals import
  nothing (covers 00209's six incomplete-pointee declarations). Located
  rejections pinned by test: binding a global pointer to a local object
  — the borrow-escape rustc would refuse, rejected at the binding site —
  plus multi-object regions, copying a global pointer, passing one to a
  function (the callee would see the staged copy), address-of, string
  literals, null constants, multiple allocation sites, and external
  linkage in a multi-TU project. 00040 and 00045 and 00149 pass and are
  in the manifest — ledger 178 -> 181 passed / 39 unsupported /
  0 miscompiled. 00209 remains blocked, no longer on its pointer
  globals: after the pointer-to-fn-ptr parameter and fn_ptr slice
  extensions landed here (pointers-fnptr-slice.c), it now rejects at
  "00209.c:24:10: error: unsupported: call with arguments through a
  function pointer without a prototype" — f1 calls through the K&R
  `int (*)()` typedef `fptr1`, a documented fn-pointer by-design
  rejection (C99-46 scope, not CTS-P4).
  (test/Import/C/globals-pointer.c, globals-pointer-invalid.c,
  pointers-fnptr-slice.c; rustc-level differential
  test/EndToEnd/pointers-global.c with data-dependent cursor updates
  across calls)
- [ ] CTS-P5 (2) Pointer-to-pointer values (`&p`, `**p`): second-order
  cursors over a region whose elements are themselves (base, cursor)
  pairs (C99-43).
  (00005.c, 00020.c)
- [ ] CTS-P6 (2) Pointers into global aggregates: same borrow-escape
  problem as CTS-P4; a global array base must be readable/writable
  through an index cursor without holding a borrow across statements.
  (00181.c, 00217.c)
- [ ] CTS-P7 (2) One pointer ranging over several objects (`p = &x;
  ... p = &y;`): PointerRegionAnalysis unions the objects into one
  region today and rejects; needs either region materialization (copy
  both objects into one backing array) or an enum-of-bases cursor.
  (00077.c, 00172.c)
- [x] CTS-P8 (1) NULL data-pointer constants: an Option-of-cursor model
  mirroring the fn_ptr None mapping; interacts with CTS-P3.
  (00171.c)
  (Done: a region that sees a null pointer constant is nullable instead
  of invalidated; each of its pointers carries the Option discriminant in
  a promotable memref<i1> "non-null" flag cell — NULL assignment stores
  false, an address binding stores true, `p = q` copies the source flag,
  and null-checks (`if (p)`, `p == 0`, `p != NULL`) read it, folding to
  constants for statically non-null pointers. A dereference of a
  possibly-null pointer is guarded by assert!(flag, "null pointer
  dereference") — C null-deref is UB, so the deterministic panic is a
  legal refinement per the fn_ptr expect precedent. CTS-P3 interaction:
  only the null-constant idiom itself is modeled; general
  integer-to-pointer traffic stays rejected, as do passing, ordering,
  differencing, and same-region comparison of possibly-null pointers,
  nullable string-literal regions, and dereference of a pointer that is
  only ever null; nullable regions are excluded from Phase-4 owner
  promotion (bare i64 cursor arguments cannot carry the discriminant).
  Import shapes in test/Import/C/pointers-null.c, rejections in
  test/Import/C/pointers-null-invalid.c, rustc-level differential with a
  data-dependent null path in test/EndToEnd/pointers-null.c, ledger
  00171.c.)

### Records and symbol namespaces (16 tests)

- [x] CTS-R1 (5) Bare anonymous struct types (no tag, no typedef name):
  synthesize a stable name (e.g. `Anon<n>` keyed by shape) and reuse the
  C99-6 dedup machinery; today only typedef'd anonymous structs import.
  (00017.c, 00043.c, 00047.c, 00118.c, 00120.c)
  (Done: `importRecord` assigns `Anon<n>` names keyed by the C99-6
  field-shape serialization — the counter only orders first encounters,
  so the same anonymous shape in any TU maps to one Rust type and
  distinct shapes never collide; the key map is consulted only for
  anonymous records, so an anonymous struct matching a named struct's
  shape keeps its own type (C type identity is by declaration). A value
  of anonymous enum type maps to plain `i32`, unlocking 00120's
  anonymous-enum member. All five tests pass and are in the manifest —
  ledger 164 passed / 56 unsupported / 0 miscompiled. Pinned by
  test/Import/C/structs-anon-bare.c (distinct shapes get distinct names,
  repeated shape shares one struct_def, named-vs-anonymous shape match
  stays two types, cross-TU dedup, anonymous-enum member) and
  test/EndToEnd/structs-anon.c (differential member reads/writes).)
- [x] CTS-R2 (2) Unnamed struct members (anonymous member injection —
  C11 6.7.2.1p13 anonymous struct/union members whose fields join the
  parent's namespace): flatten fields into the parent struct_def with
  mangled names, or reject-by-design with a note.
  (00046.c, 00050.c)
  (Done: an anonymous struct member's fields are injected into the
  parent struct_def under their own spellings — no mangling is needed
  because C11 puts them in the parent's member namespace, so Sema has
  already enforced uniqueness (a collision is a located clang error).
  Both target tests also contain anonymous UNION members; the exactly
  representable subset is implemented: an anonymous union member whose
  arms each flatten to one leaf of one identical type becomes a single
  storage slot named after the first leaf, every arm's spelling
  aliasing it — exact because reading any union member with the type of
  the last store yields that stored value (C99 6.5.2.3). Every other
  union — mixed-type arms, an arm wider than one slot, and all named or
  bare union types — keeps the located union-type rejection (CTS-R3).
  Member access skips Sema's implicit intermediate anonymous access and
  selects the flattened (alias-resolved) leaf on the parent place;
  block-scope initializer lists recurse onto the parent place with a
  union's nested list landing on its active arm's slot; constant global
  initializers convert along the C field structure so brace-elided
  values, zero-filled tails, and union slots produce the flattened
  attribute list. Distinct from CTS-R1's bare anonymous struct
  declarations, whose shape-keyed Anon naming is untouched.
  Traceability: importer lib/ImportC/ImportC.cpp (collectRecordFields,
  anonymousUnionArmLeaf, flattenedFieldName, unionSlotStorage,
  structDefRecords, emitRecordInitFields, emitRecordInitField,
  convertRecordAPValue, convertAnonymousSlotInit, and the anonymous
  skip in the member-access lvalue path); tests
  test/Import/C/structs-anon-member.c (two-level flattening, union slot
  aliasing, global/local initializer shapes),
  test/Import/C/structs-anon-member-invalid.c (mixed-type arms, wide
  arm, named union, parent-vs-member spelling collision — all located),
  test/EndToEnd/structs-anon-member.c (differential). Both tests pass
  and are in the manifest — ledger 190 passed / 30 unsupported /
  0 miscompiled.)
- [ ] CTS-R3 (3) Unions (C99-44): design decision required — safe Rust
  has no untagged unions without unsafe; candidates are a data-carrying
  enum when all accesses are type-consistent, or byte-array storage with
  typed accessor helpers for real type punning.
  (00042.c, 00210.c, 00218.c)
- [x] CTS-R4 (2) Block-scope struct declarations shadowing an outer tag
  (same tag `T`, different shape, inner scope): the importer's per-name
  shape dedup misreads this as a cross-TU conflict; record keys need
  scope depth, and the inner type needs a distinct Rust name.
  Landed: record identity is the defining `RecordDecl` (clang has already
  resolved tag scoping), so the name-keyed cross-TU shape dedup now
  applies to file-scope records only; each block-scope definition — even
  a same-shaped one, per C99 6.2.1 — emits its own struct_def under the
  function-local-static mangling convention `<function>_<tag>`
  (`_<n>`-suffixed when taken). Traceability: importer
  `lib/ImportC/ImportC.cpp` (`importRecord`, `emittedRecordName`,
  `localRecordNames`); tests test/Import/C/structs-shadow.c (shadowing,
  same-shape, double-shadow, tag-only), test/Import/C/
  multi-tu-struct-conflict.c (genuine file-scope cross-TU conflict keeps
  its diagnostic), test/EndToEnd/structs-shadow.c (differential);
  manifest ratcheted 159 -> 161.
  (00044.c, 00053.c)
- [ ] CTS-R5 (3) C's separate tag/ordinary namespaces (`struct a` and a
  global `a` coexisting, or a static local colliding with the mangled
  `<fn>_<name>` scheme): Rust has one namespace per kind but the emitter
  uses one symbol table; mangle tags (e.g. `Struct_a`) or detect-and-
  rename on collision.
  Detect-and-rename landed in the importer: a per-TU pre-pass
  (`collectOrdinaryNames`) records every name the ordinary namespace will
  claim (functions after `main`/TU-tag mangling, file-scope variables,
  function-local statics under their `<fn>_<name>` mangle), and
  `structSymbolName` keeps the readable tag when free, renaming
  deterministically to `Struct_<tag>` only on actual collision — order
  independent, cached per definition. If the renamed spelling is also
  claimed, the import is a located rejection
  (test/Import/C/structs-tag-namespace.c, -invalid.c; differential
  test/EndToEnd/struct-tag-namespace.c). 00129.c and 00219.c pass and are
  in the ratchet manifest; 00204.c clears its namespace blocker but hits
  a second unsupported construct before the predicted printf shapes:
  "00204.c:36:28: error: unsupported builtin type 'long double'" —
  box stays unticked until long double (and then `%.Ns`/`%llx`) land.
  (00129.c, 00204.c, 00219.c)
- [ ] CTS-R6 (1) Empty structs (`struct T {};` — a GNU/C2x shape clang
  accepts): emit a unit-like Rust struct; today "struct with no
  members" rejects.
  Empty-struct support landed: the importer accepts a field-less
  record, struct_def permits empty field arrays, and the emitter prints
  `struct T {}` (declaration/copy/default via the usual derives;
  test/Import/C/structs-empty.c, Dialect ops.mlir, Target memory.mlir).
  00216.c stays blocked on its next feature — the flexible array member
  `struct S s[];` rejects with "unsupported: non-constant array size"
  (00216.c:46) — so it remains off the manifest.
  (00216.c)

### Statements and expressions (10 tests)

- [x] CTS-S1 (2) Compound assignment with operand promotion
  (`char/short x; x += wider;`): lower as load, widen-cast, operate,
  narrow-cast, store. Must respect the zero-vs-sign-extension trap
  documented for the pipeline (adversarial negative/width-extreme
  differential tests required).
  Lowering implemented: `buildCompoundAssignValue` widens the loaded LHS
  to Sema's `getComputationLHSType`, operates, and narrows the result
  back to the LHS storage type, with both casts routed through the C99-3
  conversion machinery (`emitrust.cast` when either side is unsigned,
  arith ext/trunc between signless types, extf/truncf between float
  widths, sitofp/fptosi for the int-accumulator-with-float-RHS shape;
  `_Bool` endpoints stay rejected with a located diagnostic). Covers
  locals, the direct-global fast path, and value-position uses.
  Adversarial differential tests in test/EndToEnd/compound-promote.c:
  signed/unsigned char and short `/=`, `%=`, `>>=` on top-bit-set values
  (each prints differently if the widen picks the wrong extension),
  `+=`/`-=`/`*=`/`<<=` overflowing the narrow type in both directions
  (wrap-on-narrow), short -= long (the 00111.c shape), int accumulator
  with long long RHS and long long shift amount, float += double (the
  00174.c shape), and int *=/= double truncation toward zero. Both tests
  pass and are in the ratchet manifest: 00174.c's former second blocker
  ("00174.c:45:19: error: unsupported: call to 'sin' declared in a
  system header") was cleared by the hosted `sin` -> `f64::sin` mapping
  (C99-48).
  (00111.c, 00174.c)
- [x] CTS-S2 (2) Switch bodies that are not plain compound statements
  and case labels nested inside inner statements (Duff-adjacent,
  C99-32 note): requires emitting switch dispatch as cf-level branches
  into arbitrary statement positions rather than the structured match
  lowering; goto's labelBlocks machinery (C99-33) is the likely vehicle.
  Landed: a dispatch fallback lowering (`emitDispatchSwitch`) used when
  the body is not the plain shape (non-compound body, statement before
  the first label, or a case/default label nested inside an inner
  statement): every label of the switch (clang's
  `SwitchStmt::getSwitchCaseList`, which covers buried labels but not
  those of nested switches) becomes an ordinary block registered up
  front — the goto labelBlocks pattern, keyed by the label statement —
  the dispatch is one `cf.switch` to those targets, and the body is
  emitted in source order with each label redirecting emission into its
  block, so fall-through into and out of loop bodies (Duff's device) is
  plain block fall-into; lift-cf-to-scf absorbs the possibly
  irreducible result exactly like goto into a loop. Variable places
  under the dispatch are hoisted to the entry block (the dispatch may
  jump over declarations, like goto). The structured lowering stays the
  default for plain bodies, and GNU case ranges stay rejected with a
  located diagnostic in both paths. Differential coverage sweeps Duff's
  device over every entry residue and data-dependent trip counts, case
  labels in both arms of an if, INT_MIN/INT_MAX and negative case
  values with no-match values on both sides (FR-25), and a for-loop
  break under a case entered mid-loop. Both tests pass and are in the
  ratchet manifest.
  (test/Import/C/switch-dispatch.c, switch-invalid.c,
  test/EndToEnd/switch-dispatch.c)
  (00051.c, 00143.c)
- [x] CTS-S4 (2) Multi-dimensional arrays (C99-41): nested
  `emitrust.array` types, nested ArrayAttr initializers (the C99-11
  file-scope machinery already recurses), and row-major subscript
  lowering; mapType currently rejects the type before anything else
  runs.
  Landed: `!emitrust.array` elements may nest (emitted `[[T; N]; M]`),
  mapType recurses, the APValue global-initializer converter and the
  local init-list walker already recursed once the type mapper let them,
  and direct `a[i][j]` chains one `emitrust.subscript` per level. The
  pointer decomposition gained a flat row-major cursor into
  multi-dimensional bases: `&arr[i][j]` folds to `i*N + j`, a row
  pointer's subscript scales by the row span, and place materialization
  peels one array level per subscript by div/rem on the cursor. Walking
  arithmetic on row pointers (`++`, `+ n`, `+=`, difference) stays a
  located rejection (CTS-P scope), as do slices of rows. Both tests pass
  and are in the ratchet manifest.
  (test/Dialect/EmitRust/types.mlir, invalid.mlir,
  test/Import/C/arrays-multidim.c, pointers-local-invalid.c,
  test/EndToEnd/arrays-multidim.c)
  (00130.c, 00151.c)
- [ ] CTS-S5 (1) Variable-length arrays: conflicts with the
  deterministic/bounded design philosophy; recommend documenting as a
  permanent by-design rejection rather than implementing.
  (00207.c)
- [x] CTS-S6 (1) Integer-to-enum conversion (the reverse of C99-5):
  needed a design decision — `#[repr(i32)]` enums admit no safe `from`
  without a match table; candidates were a `fn <Enum>_from_i32`
  exhaustive-match helper, or rejection.
  Landed with the **preserved-value policy**, not the panic-refinement
  helper: 00170.c stores 12 into `enum fred` (matching no declared
  enumerator, values {0..3, 54, 73..75}) and prints it, which C defines as
  value-preserving (C99 6.7.2.2: the object holds any value of the
  underlying type), so an exhaustive match over declared discriminants
  cannot represent the result and a panic arm would abort a defined C
  program. The representation was therefore changed to a value-preserving
  open enum: `emitrust.enum_def` (now carrying an `unsigned_underlying`
  marker mirroring clang's underlying-type choice) emits a
  `#[repr(transparent)]` tuple struct over the storage integer (`i32` or
  `u32`) with one associated constant per enumerator and a Default impl
  returning the first variant; nominal typing, enumerator paths
  (`Fred::C`), `==`/`!=`, and `Name::default()` are unchanged.
  Int-to-enum lowers to `emitrust.cast` to the enum type (total,
  rendered `Fred(v as u32)`), enum-to-int renders `.0 as`, and the new
  `emitrust.enum_raw` place op supports 00170.c's other blocker, C's
  enum/underlying-type pointer compatibility (`deref(&e)` with a
  `unsigned int *` parameter borrows `&mut e.0`). Float-to-enum stays a
  located rejection.
  (test/Import/C/enum-from-int.c, enums-invalid.c,
  test/Target/Rust/match.mlir, test/Dialect/EmitRust/ops.mlir,
  invalid.mlir, test/EndToEnd/enum-from-int.c)
  (00170.c)
- [ ] CTS-S7 (2) `(void)` casts and void-typed contexts (evaluate and
  discard, `void` in a statement-expression position): map to an
  expression statement / `let _ =` discard; today "unsupported cast
  (ToVoid)" / "unsupported builtin type 'void'".
  Partial: ToVoid casts now evaluate the operand as an expression
  statement (a side-effect-free operand emits nothing) and void-typed
  conditionals in statement position lower as if/else diamonds, which
  unlocks 00212.c; 00213.c clears its void blockers but hits a second
  blocker, GNU statement expressions ("unsupported expression:
  StmtExpr"), plus goto-into-dead-code shapes (CTS-S2 territory).
  (00212.c, 00213.c)

### Functions and linkage (2 tests)

- [ ] CTS-F1 (2) Variadic calls and variadic function-pointer types
  beyond the printf/puts intrinsics (C99-37): design decision needed
  (safe Rust has no C-style varargs; candidates are arity-specialized
  monomorphization at call sites, or rejection).
  (00186.c, 00189.c)

### Hosted library surface (5 tests)

- [x] CTS-L1 (2) string.h subset — at least `strcpy` into a char array
  (C99-48): safe helper over `&mut [i8]` mirroring `__emitrust_cstr`;
  bounds are compile-time known array sizes, so no unsafe needed.
  DONE: definition-less strcpy/strncpy/strcat/memset/memcpy (statement
  position), strcmp/strncmp/memcmp (value position), strlen widened to
  char arrays, and strchr/strrchr (feeding printf %s and null
  comparisons via a found-index-or-minus-1 lowering) all lower by name
  to one-per-module safe helpers over `&[i8]`/`&mut [i8]` slices of the
  argument regions; same-object memcpy takes one mutable borrow plus two
  cursors (`copy_within`), and same-object copy sources, literal-region
  destinations, copy-result value uses, and uncurated <string.h>
  functions (strstr, ...) keep located rejections. printf %s also gained
  the `&arr[i]` element-pointer shape (00180.c). Both tests pass and are
  in the ratchet manifest.
  (test/Import/C/strings-hosted.c, strings-hosted-invalid.c,
  test/EndToEnd/strings-hosted.c)
  (00179.c, 00180.c)
- [x] CTS-L2 (1) printf %s of a `char *` function parameter: extend the
  C99-28 %s shapes to accept the FR-28 `mut_ref<slice<i8>>` parameter
  class (slice + `__emitrust_cstr`). Landed with two enabling pieces the
  test also needed: a string-literal argument to a slice parameter
  (fresh mutable per-call backing; copies are unobservable because
  writing a literal is UB), and C main's (int argc, char **argv) form —
  argc imports as i32 (the crate wrapper passes the process argument
  count via args_os), argv is dropped with a located rejection on any
  use. (00200.c; test/Import/C/printf-slice-param.c, main-args.c,
  test/EndToEnd/percent-s-param.c)
- [ ] CTS-L3 (2) String-literal and other initializers for
  pointer-typed objects (`char *s = "…"` at file scope, struct fields):
  a string-literal region base for a *global* pointer needs a module-level
  read-only backing (the CTS-P1 backing is function-local), which CTS-P4
  deliberately left rejected. Post-CTS-P4 diagnostics: 00089 rejects at
  "00089.c:10:7: error: unsupported: global initializer for this type"
  (pointer struct field, CTS-P2 adjacency) and 00220 at "00220.c:7:19:
  error: unsupported: string literal initializer for this type"; a bare
  `char *s = "…"` at file scope rejects in importPointerGlobal, and a
  body binding as "global pointer bound to a string literal"
  (globals-pointer-invalid.c).
  (00089.c, 00220.c)
Not itemized above: printf precision (`%.3s`) and long-long length
specifiers (`%llx`, `%10Ld`) remain outside the C99-47 grammar, but no
test is sole-blocked on them today (00182.c already passes; with CTS-R5's
rename landed, 00204.c now rejects on `long double` at 00204.c:36:28
before reaching printf) — they surface behind long double on 00204.c.

## Non-Goals for the MVP

Generics, lifetimes beyond simple references, traits and impls, pattern
matching beyond literal match arms, data-carrying enums (C-like unit-variant
enums are supported), error-handling sugar, and expression trees (every
value is a named let binding; no inlining of subexpressions). On the C side
the importer rejects, with located diagnostics: computed goto (plain
goto/labels are supported per C99-33), unions, bitfields,
int-to-enum conversions, pointer-to-pointer values, pointer struct fields,
NULL data pointers, void* casts, malloc and friends
(pointer arithmetic, pointer locals, and pointer/array parameters are now
supported through the FR-28 decomposition; pointer-typed globals with one
global region base are supported per CTS-P4, including its carve-out
promoting a single constant-size calloc/malloc site bound to a global
pointer into a static backing array),
multi-dimensional arrays, sizeof/_Alignof of
variable-length-array/incomplete/function operands, conditional operators
with non-scalar results, variadic definitions, and `char *` variables
bound to string literals (aggregate initializer lists are supported per
C99-11/12, `char s[] = "..."` and the printf/puts %s shapes per
C99-28/47). These are natural follow-ons; the emitter's
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
