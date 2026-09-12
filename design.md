# EmitRust: An MLIR Dialect for Emitting Rust Source Code

## Product

The product is a C-to-Rust porting tool: `emitrust-cc` (and its drop-in
compiler shim `emitrust-clang`) converts C programs and projects into
safe, readable Rust crates whose observable behavior is byte-identical
to the original. The user is the owner or future maintainer of a C
codebase who wants it living in Rust — all at once for small programs,
incrementally for large ones — without trusting anything but their own
eyes and their own test oracle.

Everything in this document below this section is an implementation
detail of one of the following user-story epics. The epic is the unit of
product intent; the numbered FRs are the units of verification that
implement them; the design principles, dialect contract, and roadmaps
are how, not why.

- EPIC A — Faithful conversion. As the owner of a C program, I can
  convert it to a Rust crate that produces byte-identical stdout and
  exit status, so the port needs no leap of faith. Acceptance: the
  EndToEnd differential suite, the c-testsuite conformance ledger, and
  the three-way fuzz contract — byte-exact, never weakened.
  Implemented by: FR-1..FR-25 (dialect, emitter, importer, driver,
  differential oracle), the C99 support roadmap, the c-testsuite
  checklist.
- EPIC B — Honest limits. As a user, when something cannot be
  converted I get a located diagnostic naming the construct and the
  root-cause blocker — never silently wrong code — plus a ledger of
  exactly what is and is not covered. Acceptance: pinned rejection
  wordings, blocker-attribution tags, ledger/ratchet reports that may
  only improve. Implemented by: FR-9, FR-19, FR-24, FR-49, the
  rejection ledger and ratchet manifests.
- EPIC C — My build, unchanged. As a project owner, I point the build
  system I already have (make, compile_commands.json, a Linux kernel
  build) at the shim compiler and get per-TU artifacts and a link-step
  whole program, without restructuring anything. Implemented by:
  FR-26, FR-27, FR-45, FR-56, FR-57, FR-58, FR-60.
- EPIC D — Incremental porting. As the porter of a codebase too large
  to convert in one shot, I get the largest provably-working subset as
  a crate today, a frontier report of what blocks the rest, and a
  guarantee the ported share only ever grows. Implemented by:
  FR-40..FR-44, FR-50, FR-51, FR-52, the kernel-corpus ratchet.
- EPIC E — Rust a person would write. As the future maintainer, the
  output is zero-unsafe, warning-clean, idiomatic Rust — slices and
  indices instead of pointers, enums instead of tag ints, owner
  structs, expression-oriented style — so the port is a starting
  point, not a museum piece. Implemented by: FR-28..FR-39, FR-53
  warning-clean codegen, FR-61 rustacean-style emission.
- EPIC F — Modern architecture. As the future maintainer, clustered
  global state becomes actor-owned state behind explicit messages
  (default-on where certified, threaded/async as opt-in modes, a
  pluggable partitioner for actor responsibility), so the port is
  concurrency-ready — while observable behavior never changes.
  Implemented by: FR-30 owner structs, FR-59 workspace partitioning,
  FR-62 and its slice list.

Every epic's ultimate acceptance criterion is EPIC A's oracle: no
feature, style, or architecture change is accepted on any evidence
weaker than byte-identical behavior.

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
| cast | one operand, one result; a bool result type is rejected (no Rust as-cast produces a bool); an enum result requires a non-i1 integer source and renders as a tuple-struct construction (`Color(x as u32)`) | as-cast expression |
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

> **Queue index.** The open items below (and in every later FR/wave
> section) are indexed in `docs/plans/backlog.toml` with status, rank and
> dependency edges; query with `python3 docs/plans/plan.py next|brief|json`.
> design.md remains the authoritative evidence ledger — the index carries
> no rationale, only ordering — and `plan.py check` (run by the fast lit
> tier) fails the gate if the two drift.


Every FR below is an implementation detail of one of the product epics
(see "Product" above); the epic carries the product intent, the FR
carries the verification. Check a box only when the referenced
regression test passes under ninja check-emitrust in the pinned dev
shell. Traceability: each requirement lists the lit test file(s) that
validate it.

