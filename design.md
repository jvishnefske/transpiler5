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

  Differential fuzzing (test/Fuzz/): a seeded generator (genprog.py)
  composes 2-5 feature templates per program — union puns, byte
  reinterprets, cell-slice globals, void*/member-base/null-ternary
  pointers, variadic + sprintf, StmtExprs, fn-ptr devirtualization
  (including local void* fn-holders with cast-calls), global-return
  chains, int-carrier/expect/missing-return, writeback
  ordering (RHS/index calls mutating a distinct subobject of the
  assigned global), C99-45 bit-fields (mixed runs, sign/zero extension,
  RMW flags, value-position assignment), C99-48 FILE* round-trip I/O
  over per-seed scratch files run in per-seed cwds, byte-array union
  arms accessed only through the integer arm, and dead-VLA elision
  noise, each modeled on a test/EndToEnd differential test — into UB-free C11 programs whose
  indices and branch conditions are runtime-computed, with printf digests
  at multiple points and a computed exit code in 0..250. The comparison
  is three-way: genprog carries an exact per-template Python evaluator
  (explicit wrapping arithmetic — 32-bit two's-complement int/unsigned,
  64-bit unsigned long, little-endian byte puns asserted at import), so
  every seed also has a generator-predicted (stdout, exit) pair; no
  template is oracle-exempt. differ.py runs each program through clang
  and emitrust-cc --emit=crate --build and classifies: PASS (oracle ==
  native == transpiled) / UNSUPPORTED (rejection is
  never a failure) / MISCOMPILE (transpiled diverges while native
  matches the oracle — always fatal under --fail-on-miscompile)
  / GENERATOR_ORACLE_BUG (native disagrees with the generator's own
  expected output: generator UB or evaluator drift, always fatal like
  HARNESS_BUG) / HARNESS_BUG (native leg broke: generator defect,
  always fatal).
  Determinism contract: a program AND its expected-output artifact are a
  pure function of (seed,
  GENERATOR_VERSION); same seed, same bytes — every finding reproduces
  from its seed number. Triage protocol: shrink with minimize.py
  (delta-debug over template instances, then value-pool choices,
  three-way at every step), pin the
  minimized program as test/EndToEnd/<feature>-fuzz-<seed>.c, fix the
  compiler, re-run the seed range. fuzz-smoke.c (seeds 1-16, REQUIRES:
  cargo) keeps the harness green inside check-emitrust; big campaigns run
  via fuzz_differential.py (see test/Fuzz/README.md).

  Pipeline-level differential abstract interpretation (landed): the
  emitrust-range-refinement-check pass runs MLIR's integer-range dataflow
  analysis (IntegerRangeAnalysis over InferIntRangeInterface) before and
  after convert-to-emitrust and compares the derived ranges at the
  observation points. Two input modes: primary (a plain post-mem2reg
  module — the pass analyzes it, clones it, runs convert-to-emitrust on
  the clone in a nested pass manager, and re-analyzes) and secondary (a
  module holding exactly two nested builtin.module ops tagged
  emitrust.stage = "before"/"after", compared directly with no internal
  conversion). Matching convention: observation points are keyed by
  (enclosing function symbol, per-function occurrence index of the
  emitrust.call_opaque "print!" call or of the return terminator, operand
  index); occurrence indices restart per function, so the comparison is
  positional per function, not global.

  Per-op transfer-function coverage, three classes:
  - Trivial with interface (exact or refining): emitrust.constant
    (singleton), emitrust.cmp (result in 0..1, refined to a constant when
    the operand ranges decide the predicate; ordered predicates compare
    unsigned exactly when the operand type is ui<N>), emitrust.cast
    (int-to-int: same width preserves the bit pattern, narrowing
    truncates, widening zero-extends from ui<N>/i1 and sign-extends from
    signless i<N> — the ui/i signedness mapping that makes the checker
    target sign-extension bugs), emitrust.select (branch union, or the
    taken branch when the condition is statically known), and the binary
    ops add, sub, mul, div, rem, and, or, xor, shl, shr (signedness
    selected by type for div/rem/shr).
  - Lossy top (interface present, deliberately unconstrained):
    emitrust.literal (opaque text) pins its integer result to the full
    type range; emitrust.enum_raw constrains nothing (its result is a
    place, not an integer).
  - Opaque (no interface, top by framework default or by absence):
    emitrust.load and all other memory/place ops, call_opaque results,
    global_load, method_call, and values inside emitrust region ops whose
    lattices stay uninitialized — all read as TOP at observation points.

  Wrap semantics: every transfer function delegates to the
  mlir::intrange::infer* helpers with OverflowFlags::None — never
  nsw/nuw — because unsigned emitrust arithmetic is emitted as Rust
  wrapping_* calls and signless arithmetic mirrors two's-complement
  machine arithmetic; defined wrap-around on either side of the
  comparison therefore never manufactures a false positive
  (test/Conversion/range-refinement/positive-wrap.mlir pins this).

  Verdict semantics: EMPTY INTERSECTION ONLY. A hard, located error —
  "range refinement violation: '<fn>' print operand <k> (or return value
  <k>) has pre-conversion range [..] disjoint from post-conversion range
  [..]" — is emitted only when the pre and post ranges are disjoint under
  BOTH the unsigned (umin/umax) and the signed (smin/smax) interpretation
  of ConstantIntRanges' paired-bounds intersection. This is the
  deliberately conservative lattice choice: each interpretation's bounds
  are individually sound, but ranges built from a single interpretation
  carry an artificially widened complement, so requiring both empty is
  immune to one-sided widening and cannot flag two sound ranges that
  share a value. Mere containment failure (post not within pre, e.g. a
  memory load that reads TOP after conversion) is silent, tracked only by
  the containment-failures pass statistic; uninitialized or absent
  lattice values are TOP.

  Honest coverage statement: the checker proves range consistency, not
  value identity. It targets the sign-extension/zero-extension and
  constant-folding miscompile class (a ui<N> value re-interpreted signed,
  a wrong folded constant — anything that moves an observation point's
  range off the original), and it covers ALL values of those ranges, not
  sampled seeds. It does NOT catch value-identity bugs whose wrong value
  still lies inside the pre-conversion range — in particular the
  lost-copy/swap class of SSA-destruction bugs (both swapped values
  inhabit the same range), which remains the province of the differential
  fuzzer and the pinned EndToEnd lost-copy tests.
  (test/Conversion/range-refinement/positive-refine.mlir,
  positive-wrap.mlir, positive-memory-top.mlir, negative-pair.mlir,
  negative-return.mlir)

  Wiring (landed): emitrust-cc gains --check-range-refinement (off by
  default), which inserts the checker into the pinned pipeline
  immediately before convert-to-emitrust — i.e. after lift-cf-to-scf and
  its canonicalize, on the conversion's exact input. That placement, not
  the post-mem2reg point, is load-bearing: the pass's primary-mode
  internal clone pipeline runs only convert-to-emitrust, which marks cf
  illegal but cannot lift it, so unlifted cf.br/cf.cond_br in pre-lift
  IR fail the internal pipeline on every branching program (observed:
  100% spurious compile failures when wired pre-lift). The pass is
  observational — it clones and converts internally — so the main
  pipeline continues unchanged after it; a violation fails the compile
  with the pass's located "range refinement violation" diagnostic.
  Fuzz-harness mode: fuzz_differential.py/differ.py --range-check adds
  the flag to every emitrust-cc invocation and classifies a compile
  failure whose stderr carries "range refinement violation" as the
  hard-fail class RANGE_VIOLATION (reported like MISCOMPILE, artifacts
  saved, fatal under --fail-on-miscompile); all other compile failures
  stay UNSUPPORTED, and the generator is untouched (GENERATOR_VERSION
  unchanged). Because every generated program is correct by
  construction, any RANGE_VIOLATION is a checker false positive.
  Shakedown: a 500-seed checker-on campaign (seeds 5000-5499, jobs 8)
  came back seeds=500 pass=500 unsupported=0 miscompile=0
  range_violation=0 oracle_bug=0 harness_bug=0, oracle agreement
  500/500. Measured overhead: a 100-seed run (seeds 5000-5099, jobs 8)
  took 81.9s plain vs 81.6s checker-on — the checker's compile-time
  cost is below run-to-run noise (cargo build dominates each seed).
  fuzz-smoke.c carries a second RUN line (seeds 1-6, --range-check)
  guarding the wiring inside check-emitrust.

  Pipeline-level differential CONCRETE interpretation (landed): the
  emitrust-value-identity-check pass is the value-identity sibling of the
  range checker, sharing its two-stage input scaffold (primary mode:
  interpret the input module, clone it, run convert-to-emitrust on the
  clone in a nested pass manager, interpret the clone; secondary mode:
  two nested builtin.module ops tagged emitrust.stage = "before"/"after"
  compared directly — the mode detection and clone-and-convert step are
  factored into DifferentialStages.h so the two checkers cannot drift).
  Entry points are the parameterless functions of the pre-conversion
  stage (in practice c_main and parameterless helpers); each is
  interpreted independently by a bounded environment-based evaluator
  (integers as APInt keyed by SSA value; emitrust.let and scalar
  emitrust.variable results are mutable slots that later reads observe;
  fresh frame per func.call / resolved call_opaque, call depth capped).
  Observations — every emitrust.call_opaque "print!" operand and every
  executed return operand, callees included — are recorded in execution
  order and compared pairwise; the first divergence per entry point is a
  located hard error with values in signed decimal: "value identity
  violation: '<fn>' print operand <k> (or return value <k>) at
  occurrence <n> observed <a> pre-conversion but <b> post-conversion".

  Coverage/skip semantics: the interpreted op set is deliberately
  bounded — arith integer ops, scf.if/while/for/index_switch with
  yields, cf.br/cond_br, emitrust
  constant/cast/cmp/binaries/select/let/assign/load + scalar variables,
  emitrust if/loop/for/switch/break/continue, calls into module
  functions (a post-conversion call_opaque callee is resolved against
  the module symbol table FIRST; only an unresolvable non-"print!"
  callee is unsupported). Reaching anything else — subscripts, members,
  cells, globals, floats, opaque constants, FILE helpers, an exhausted
  step budget (10M steps; a 4096-iteration loop verifies comfortably,
  test/Conversion/value-identity/positive-loop-budget.mlir) — SOFT-SKIPS
  that entry point silently: no diagnostic, the pass succeeds, only the
  skipped-entry-points statistic counts it (compiled out of release
  builds like all llvm::Statistic). Poison policy: ub.poison
  materializes a poison marker that may flow through rebindings and
  stores, but an entry point that actually EVALUATES poison (arithmetic,
  comparison, branching, or an observation) is soft-skipped —
  interpreting poison as the zero the conversion folds it to would
  vacuously "verify" programs whose pre-conversion behavior is
  undefined.

  Honest complementarity statement: this mode executes one concrete run
  per entry point, so unlike the range checker it proves nothing about
  other inputs — but parameterless entry points have exactly one run,
  and over that run it compares exact VALUES, which the range checker
  structurally cannot. It therefore DOES catch the lost-copy class the
  range checker is blind to: two values with identical (TOP) ranges
  where conversion observes the wrong one — the yield-rebinding swap
  shape of the project's worst historical miscompile
  (test/Conversion/value-identity/negative-value-swap.mlir pins the
  swapped-print shape, negative-return.mlir the intersecting-range
  wrong-return shape that the range checker provably ignores;
  positive-identical.mlir, positive-skip.mlir, positive-loop-budget.mlir
  pin the pass/skip semantics). Coverage probe over real post-lift
  modules (emitrust-import-c + mem2reg/canonicalize/lift-cf-to-scf/
  canonicalize on EndToEnd sources): loops, lostcopy-min,
  lostcopy-rotations, recursion — 8 of 8 entry points verified (up to
  111 observation events each); value-exprs — both entry points
  soft-skipped (array subscript places are outside the scalar-slot
  coverage), exactly the honesty the skip statistic is for.

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
  A later real-world probe over TheAlgorithms/C found a fifth silent
  miscompile class that had survived all 200 ledger vectors: the lost-copy
  problem of SSA destruction. Loop back-edges rebind all scf block
  arguments in parallel, but the SCF-to-EmitRust yield lowering emitted
  the rebinding as sequential Rust assignments, so a loop-carried copy
  cycle (the Fibonacci rotation in searching/fibonacci_search.c, and any
  swap) read a freshly clobbered carried variable. Fixed in the shared
  assignment helper of the SCF conversion: when any yielded source aliases
  a carried let written earlier in the sequence, every source is staged
  into a fresh immutable temporary before any carried let is assigned
  (parallel-assignment semantics); hazard-free back-edges keep the direct
  form. New adversarial rotation vectors cover the 2-cycle swap, the
  3-variable Fibonacci rotation, a partial cycle among non-cycling
  updates, a rotation mixed with independent accumulators, nested loops
  rotating at both levels, and a rotation feeding a data-dependent branch;
  each differentially fails against the pre-fix compiler.
  (test/EndToEnd/lostcopy-min.c, lostcopy-rotations.c,
  lostcopy-nested-branch.c, test/Conversion/SCFToEmitRust/while.mlir,
  for.mlir, test/Target/Rust/loop.mlir)
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
  S1b traversal fusion deferred: changes inter-analysis ordering (the
  Pass-A planners share their scaffold but keep separate TU walks).

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
- [x] C99-4 Plain char signedness policy, character constants, and
  escape sequences. POLICY: plain `char` is signed — the x86-64 Linux /
  clang default the differential oracle uses — so Char_S maps to
  signless i8 exactly like `signed char` (Char_U/UChar map to ui8).
  Character constants have C type int and import as the i32 constant
  clang evaluated: simple escapes (\n \t \0 \\ \' \"), octal (\012) and
  hex (\x41) forms, and — under the signed-char policy — sign-extended
  high bytes ('\xff' is -1, '\x80' stored to a char reloads as -128).
  They work in expressions, comparisons, switch case labels (via
  clang's constant evaluator), array subscripts, and global
  initializers (via APValue). Wide constants (L'') are accepted as
  their code-point value — wchar_t is int on this target, matching the
  wide-string verbatim-code-unit policy and c-testsuite 00098 — while
  Unicode constants (u8''/u''/U'', charN_t types outside the model) and
  multi-character constants ('ab', implementation-defined value) are
  rejected with located diagnostics; a multibyte source character
  ('é') never reaches the importer — clang rejects it first. String
  literal escapes ride the existing C99-28 per-byte machinery (clang
  decodes them before import): all standard escapes including \a \b \f
  \v \r round-trip byte-identically; embedded NUL and the ASCII-only
  rejection of non-ASCII bytes are unchanged.
  (test/Import/C/char-constants.c, char-constants-invalid.c,
  test/EndToEnd/char-constants.c)
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
- [x] C99-7 Type qualifiers. const maps positionally: a never-written
  const global (scalar or array) imports as a const-marked
  emitrust.global (an immutable Rust static; `static const` locals join
  via their mangled module global), string-literal backings are
  const-marked variables (immutable lets), const-qualified locals keep
  the ordinary variable/alloca lowering (writing through a const lvalue
  is already a clang frontend error, and mem2reg renders scalar SSA
  forms as immutable lets), a const value parameter is an ordinary
  by-value scalar, a const pointee (`const int *p`) classifies exactly
  like its unqualified spelling (mut_ref, slice, or cell-slice per the
  region analysis), and qualification-only pointer casts that add or
  drop const stay transparent (CTS-P2 peeling). volatile is rejected by
  policy with a located "unsupported: volatile-qualified type" wherever
  a declared type carries it at any level — locals, globals, parameter
  pointee chains, struct fields, return types (deep scan
  `hasVolatileQualifier`, plus checks in mapType, mapParamType,
  mapStructFieldType, emitLocalVar, and createGlobal) — and a cast that
  introduces a volatile pointee refuses the qualification peel so the
  site keeps a located rejection. Exception (c-testsuite 00162):
  qualifiers on a parameter OBJECT itself (`volatile int v`,
  `int x[volatile 5]`, which adjusts to `int * volatile x`) are
  body-local, never part of the function type, and accepted-and-ignored.
  restrict is accepted and ignored everywhere (a pure aliasing hint; the
  pointer region analysis is stricter than restrict), so
  restrict-qualified parameters import identically to unqualified ones.
  _Atomic is rejected with a located
  "unsupported: _Atomic-qualified type". Pre-existing const limits
  unchanged: struct- and fn_ptr-typed const globals keep the Cell
  representation (the GlobalOp const marker is scalar/array only), and a
  const global array argument to a pointer parameter keeps the CTS-P10
  located rejection (cell-slice classes require mutable global bases).
  (test/Import/C/qualifiers.c, qualifiers-invalid.c,
  test/EndToEnd/qualifiers.c)
- [x] C99-8 long double: PERMANENT documented rejection. Rust has no
  extended-precision float; a silent double mapping would change numeric
  results and break the byte-exact differential oracle for printf %Lf
  (the 00204 shape). The importer rejects with the located diagnostic
  "unsupported builtin type 'long double'" at the first use of the type.
  (test/Import/C/long-double-invalid.c; see also the 00204
  PERMANENT-OUT disposition below.)
- [x] C99-9 _Complex and _Imaginary: documented rejection — no Rust
  counterpart. _Complex reaches the importer's type mapper and rejects
  with the located diagnostic "unsupported type '_Complex double'";
  _Imaginary (optional C99 Annex G, never implemented by clang) is a
  located clang frontend rejection ("imaginary types are not supported")
  before import begins. Both are pinned build-time errors, never silent
  acceptance. (test/Import/C/complex-invalid.c)

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
  initializers (whole-struct copies; compound literals are supported per
  C99-13; string literals on char arrays are supported per C99-28) and
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
- [x] C99-13 Compound literals in expression position.
  Implemented for block-scope struct/union/array literals: the literal
  materializes as a fresh anonymous `emitrust.variable`
  (default-initialized — C99 zero fill — then the C99-11 per-element
  assigns; `(char[N]){"..."}` fills like a string-initialized array per
  C99-28) and from there behaves as an ordinary lvalue of that temp.
  Value uses load it whole (assignment right-hand side — including the
  self-referencing `s = (struct S){s.b, s.a}` swap, which reads the old
  values through the temp — by-value argument, return); member and
  subscript accesses resolve on the temp's place; a variable initializer
  or aggregate-element position initializes the target place directly
  through the literal's list (the copy source is immediately dead). A
  decayed or address-taken literal binds a synthesized backing
  declaration (one per literal, shared between the planning and emission
  analyses) as a pointer-region base like any named local: cursor walks,
  writes through the pointer, slice arguments, multi-base rebinding
  (CTS-P7), and the degenerate struct base all compose unchanged. A
  region-base temp is hoisted to the entry block (dereferences anywhere
  in the body must be dominated) and each evaluation first restores the
  type's pristine default value, so a literal bound inside a loop
  re-zeroes its holes exactly like C's fresh object per evaluation.
  Full expressions containing literals arrive wrapped in
  ExprWithCleanups, peeled as trivia (the "cleanup" is the temp's end of
  life). Owner promotion never keys on a literal base (no declaration
  statement to anchor the owner struct); such regions stay on the
  Phase-1b lowering. Rejected with located diagnostics: scalar compound
  literals, binding a global pointer to a literal (the borrow would
  outlive the block), returning a pointer into one (dangling); a
  block-scope `static` pointer initializer is rejected by clang's own
  constant-initializer check. File-scope literals keep their CTS-P4
  `<name>_backing` global path.
  (test/Import/C/compound-literals.c, compound-literals-invalid.c,
  test/EndToEnd/compound-literals.c)
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
- [x] C99-16 Variable-length arrays and variably modified types:
  documented rejection for LIVE VLAs — no fixed-size Rust counterpart,
  and VLAs are only conditionally supported in later C standards. A
  referenced VLA, or one whose size expression has side effects, rejects
  with the located diagnostic "unsupported: non-constant array size" at
  the declared variable. The sole carve-out is dead-VLA elision (landed
  2026-07-19 for 00207): an unreferenced VLA whose size expression is
  side-effect-free is elided at import and the rest of the function
  imports untouched. Both sides are pinned.
  (test/Import/C/vla-dead-elision-invalid.c rejection,
  vla-dead-elision.c acceptance)
- [x] C99-17 Flexible array members: documented rejection. A FAM
  (C99 6.7.2.1p16) gives the struct an allocation-time size the
  fixed-shape value model cannot represent (00216's first blocker). The
  importer rejects with the dedicated located diagnostic "unsupported:
  flexible array member" at the member — previously the shape fell
  through to the generic "unsupported: non-constant array size" array
  fallback. (test/Import/C/flexible-array-invalid.c; see also the 00216
  PERMANENT-OUT disposition below.)
- [x] C99-18 inline functions and the C99 inline linkage rules: a
  semantic no-op for the transpiler — the specifier is accepted and
  ignored, and every inline definition imports as an ordinary function
  with its body. The tricky C99 linkage case, a plain inline definition
  without extern (C99 6.7.4p7: an inline definition that provides no
  external definition), still hands clang's AST the full body, and in
  the merged whole-program module the single ordinary definition is the
  right shape; extern inline (the spelling that does provide the
  external definition) imports identically. static inline rides the
  ordinary internal-linkage path: bare name in a single-TU import,
  per-TU mangled in a multi-TU import so identically named static
  inline helpers in two units stay distinct (C99-38/FR-26).
  (test/Import/C/inline.c, inline-multi-tu.c, test/EndToEnd/inline.c)

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
- [x] C99-29 Float literal forms including hexadecimal float constants,
  and __func__. Every float literal spelling — decimal, leading/trailing
  dot, exponent, hexadecimal (C99 6.4.4.2), and the f/F suffixes —
  arrives as clang's evaluated APFloat, so only the value and type
  survive: float literals are f32 constants, double literals f64, and
  the file-scope APValue fold (C99-11/14) is spelling-blind too. Long
  double keeps its located builtin-type rejection under every spelling
  (decimal or hex L suffix), rejected at the literal before any
  narrowing cast. The `__func__`-family predefined identifiers
  (C99 6.4.2.2, plus __FUNCTION__ and __PRETTY_FUNCTION__) carry their
  function-name StringLiteral inside the PredefinedExpr and take the
  C99-28 literal paths unchanged: printf/puts %s arguments lower to the
  `emitrust.literal` string, a char-pointer binding is a read-only
  string-literal region over the name's bytes plus the NUL, and
  string-helper arguments (strlen, ...) slice the same backing. Located
  rejections: any use outside those positions (element access, plain
  value use) names the identifier, and the printf format position keeps
  the spelled-literal requirement.
  (test/Import/C/float-literals.c, float-literals-invalid.c,
  func-name.c, func-name-invalid.c, test/EndToEnd/float-literals.c,
  test/EndToEnd/func-name.c)

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
- [x] C99-35 Recursive and mutually recursive functions: pinned by a
  differential regression test — direct recursion at data-dependent
  depth plus a mutually recursive is_even/is_odd pair.
  (test/EndToEnd/recursion.c)
- [x] C99-36 Array parameters with decay semantics, including the C99
  static and qualifier forms inside the brackets. Every bracketed form —
  unsized, sized, the C99 minimum-length static form, and the const /
  volatile qualifier forms — adjusts to a pointer parameter in clang's
  AST (C99 6.7.5.3p7), so each rides the ordinary Phase-1b
  pointer-parameter classification: subscripted use classifies as a
  slice reference, deref-only use as a scalar reference. The bracket
  qualifiers qualify the decayed POINTER object itself, not the pointee,
  and the decomposition erases that object, so const and volatile inside
  the brackets are accepted and ignored — this leaves C99-7's policy on
  volatile OBJECTS untouched, since no volatile access ever survives to
  the emitted Rust. Exclusions keep their located pointer-parameter
  diagnostics: a multidimensional array parameter decays to a
  pointer-to-array with no slice shape, and an array-of-pointers
  parameter decays to a rejected pointer-to-pointer (CTS-P5).
  (test/Import/C/array-params.c, array-params-invalid.c,
  test/EndToEnd/array-params.c)
- [x] C99-37 Variadic function definitions and va_list: PERMANENT
  documented rejection for anything that touches va_list. Rust has no
  stable variadic ABI or safe varargs access, so a variadic DEFINITION
  whose body uses the va_list machinery (va_start/va_arg/va_copy or a
  va_list variable declaration, detected by the `bodyUsesVaList` scan)
  keeps the located rejection "unsupported: variadic function
  definition", and the va_list TYPE itself (the target's
  `__builtin_va_list` and its underlying `__va_list_tag` record) is
  rejected by the type mapper in every position — local, parameter,
  field, global — with the located "unsupported: va_list type", so a
  hand-rolled vprintf-style helper or a stray va_list local in a
  non-variadic function can never import as the target's
  register-save-area struct. That decl-site rejection makes the
  v*printf family unreachable by construction (every call needs a
  va_list argument); a v*printf call reached without one keeps the
  C99-39 system-header use rejection. Two deliberate carve-outs stand:
  (1) the CTS-F1/CTS-P9 fixed-prototype import — a variadic definition
  whose body is va_list-free can never observe its trailing arguments,
  so it imports as its named parameters only and call sites drop
  effect-free extras (an extra with side effects is rejected); (2)
  printf-family CALL SITES route through the hosted printf/puts
  machinery (C99-47/48) when the project supplies no definition. The
  recorded permanent-out example is 00204: behind its long-double
  surface blocker sits `va_arg(ap, struct s7)` — struct-typed varargs /
  HFA calling convention, fundamentally outside safe-Rust emission (see
  the 00204 PERMANENT-OUT disposition). (test/Import/C/varargs-def.c,
  varargs-def-invalid.c — va_list-using definition, va_list local in a
  non-variadic function, va_list parameter, vprintf call, side-effecting
  dropped extras; test/EndToEnd/varargs-def.c)
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
- [x] C99-42 Nested struct types and struct assignment as a whole:
  pinned — nested member types, whole-struct assignment (value/Copy
  semantics), member-of-nested writes, and assignment through a pointer
  deref, import-level and differential. (test/Import/C/structs-nested.c,
  test/EndToEnd/structs-nested.c)
- [ ] C99-43 Pointers to pointers and pointer members inside structs
  (design decision needed alongside C99-26: reference-typed struct fields
  require Rust lifetimes, which the dialect deliberately does not model;
  candidate mappings are index-based handles or ownership restructuring).
- [ ] C99-44 Unions (design decision needed: safe Rust has no untagged
  unions; candidate mappings are enums where usage is disciplined, or
  documented rejection).
  Partially landed (wave5 B1, one-slot struct model): a named or
  untagged union RecordDecl imports as a ONE-FIELD struct whose storage
  field is the first arm's leaf (name and type), generalizing the
  CTS-R2 anonymous-union slot machinery — every arm's spelling aliases
  that slot, so no non-first arm name reaches the IR. In scope: arms
  that all map to one identical type (exact by C11 6.5.2.3), same-width
  integer arms differing only in signedness (accesses through the
  differently-signed arm wrap a bit-exact `emitrust.cast` reinterpret,
  reads slot->arm and stores arm->slot), single-arm unions, unions as
  struct members, and union globals with constant initializers (the
  initializer lands on the slot like a one-field struct's). Pinned OUT
  of scope with located `unsupported: union ...` rejections: bit-field
  arms, pointer arms, integer arms of differing sizes, mixed
  non-integer (float/pointer/aggregate) multi-arm unions, and empty
  unions. Traceability: importer `lib/ImportC/ImportC.cpp`
  (`collectUnionSlot`, `flattenedFieldStorage`,
  `reinterpretUnionArmRead`/`reinterpretUnionArmWrite`, union routing
  in `mapType`/`importRecord`/`convertAPValueInit`); tests
  test/Import/C/unions.c (alias, single-arm, struct member, global
  initializer, signedness pun, untagged local),
  test/Import/C/unions-invalid.c (float arm, size mismatch, pointer
  arm, bit-field arm, empty union — all located),
  test/EndToEnd/unions.c (differential), c-testsuite 00042.c.
- [x] C99-45 Bit-fields. DECIDED: mask-and-shift accessor synthesis over
  per-run backing integers (not a documented rejection). PARTIAL —
  landed with c-testsuite 00218 (T1.2). Layout: each maximal run of
  consecutively declared bit-field members packs LSB-first in
  declaration order into a synthesized backing field `__bits<n>` (n
  counts runs from 0 across the flattened record) of the smallest
  unsigned type (ui8/ui16/ui32/ui64) holding the run's total bits; runs
  split at any non-bit-field member, and the bit-field members' own
  names never appear in the struct_def. This layout is the project's
  OWN and deliberately NOT ABI-compatible with the C compiler's
  bit-field layout — which is why `sizeof`/`_Alignof` of any type
  containing a bit-field record is a located rejection
  (`unsupported: sizeof of a struct with bit-fields`): the C layout
  number would promise an ABI the emitted Rust does not keep. Reads
  load the backing field, `emitrust.shr` by the bit offset (always
  emitted, offset 0 included), `emitrust.and` with the width mask, then
  convert: unsigned/_Bool/enum-typed fields ZERO-extend from the
  unsigned backing (the 00218 core: an `enum : 8` field holding 152
  reads back 152, never -104, regardless of the enum's own underlying
  signedness), plain-int signed fields sign-extend from their declared
  width via `arith.shli`/`arith.shrsi` by (type width - field width).
  Writes are read-modify-writes: load, clear the window with the
  complement mask, cast the RHS to the backing type (from the enum type
  for enum RHS), truncate with the width mask (always a separate step),
  `emitrust.shl` by the offset (always emitted), `emitrust.or`, assign
  the backing field; a value-position assignment stages the truncated
  post-store field value (converted like a read). Alongside: struct
  MEMBER names that are Rust keywords now mangle with a trailing
  underscore (`type` -> `type_`, 00218 declares a member named `type`)
  instead of rejecting — members only; struct/enum/function/global
  keyword names keep their rejections — and a mangle collision (`type`
  next to `type_`) is a located rejection. Out of scope, all located
  rejections: zero-width and anonymous bit-fields, runs wider than 64
  bits, bit-field arms in unions (unchanged wording), compound
  assignment / increment on bit-fields, aggregate and global-constant
  initializers touching bit-field members. Traceability: importer
  `lib/ImportC/ImportC.cpp` (`collectRecordFields` run packing,
  `bitFieldAccessInfo`, `emitBitFieldRead`, `emitBitFieldAssign`,
  `convertBitFieldFieldValue`, `createBitFieldMask`,
  `emitMemberBasePlace`, `mangleMemberName`/`flattenedFieldName`,
  `typeContainsBitField` in `emitSizeofAlignof`); tests
  test/Import/C/bitfields.c (packing, read/write accessor shapes,
  signed extension, run splitting), bitfields-invalid.c (union arm,
  sizeof), struct-member-keyword.c (mangle end-to-end),
  keywords-invalid.c (non-member keyword rejections stay),
  test/EndToEnd/bitfields-zeroextend.c (00218 core, differential),
  test/EndToEnd/bitfields-flags.c (mixed runs, RMW isolation,
  signed truncation), c-testsuite 00218.c.
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
  Definition guard: the whole by-name printf lowering applies ONLY when
  the project supplies no printf definition of its own — the same
  `!callee->getDefinition()` guard puts/putchar, the string.h surface,
  and the fn-pointer alias planner already carry. A project-supplied
  printf (any signature; <stdio.h> is never imported) imports and is
  called like any user function in BOTH statement and value positions;
  a variadic va_list-free user printf flows through the fixed-prototype
  variadic machinery (C99-37/varargs-def), dropping effect-free
  call-site extras its body cannot observe. The "printf return value
  must be unused" rejection now applies only to the hosted
  (definition-less) lowering.
  (test/Import/C/printf.c, printf-extended.c, printf-extended-invalid.c,
  strings.c, strings-invalid.c, test/Import/C/printf-user-defined.c,
  test/EndToEnd/printf-formats.c, test/EndToEnd/strings.c,
  test/EndToEnd/printf-user-defined.c,
  test/EndToEnd/printf-user-defined-nonvariadic.c)
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
  stdio FILE* streams (CTS-T1.3, 00187): a `FILE *` local declared
  uninitialized or fopen-initialized is an OWNED handle over std::fs —
  an `emitrust.variable` of the opaque `__EmitrustFile` type, a
  one-per-module enum over Null / Read(std::fs::File) /
  Write(std::fs::File) — and every stream operation borrows it `&mut`
  through a one-per-module `__emitrust_f*` safe-Rust helper (the
  sprintf/cstr helper convention; zero unsafe, every buffer access a
  bounds-checked slice index). Supported surface, sequential byte-wise
  I/O only: fopen(literal-path, "r"/"w") — literal-only paths, v1 —
  where "w" creates/truncates and a failed open is
  `__EmitrustFile::Null`, C's NULL, so `if (!f)` and null comparisons
  work through `__emitrust_file_ok`; fgetc/getc as an i32 byte with EOF
  the plain `arith.constant -1 : i32` met by `arith.cmpi` (an equality
  comparison folds a negated signed integer literal — EOF's `(-1)` — to
  a single negative constant; relational shapes keep the historical
  `0 - x` lowering pinned in enum-int.c); byte-wise
  fread/fwrite(ptr, 1, n, f) over char-region slices returning the
  size_t count through the strlen-style i64 helper + cast convention;
  fgets(buf, size, f) reading at most size-1 bytes, stopping after
  '\n', NUL-terminating, whose helper returns -1 for C's NULL so the
  pinned `while (fgets(...) != NULL)` and bare truth tests fold to an
  index comparison (the strchr convention); and fclose, which resets
  the handle to Null so the SAME variable is reassignable by a later
  fopen (00187's serial open/close cycles). UB-refinement stance: fopen
  failure takes C's NULL path; reading/writing a Null or
  wrong-direction handle, I/O errors beyond EOF, and out-of-range
  counts are C undefined behavior refined into deterministic panics.
  Located rejections pin the slice boundary: file positioning
  fseek/ftell/rewind (streams are sequential-only), fopen modes other
  than "r"/"w", fprintf to a real FILE* stream (only the devirtualized
  stdout form), fread/fwrite element sizes other than 1 (byte-wise
  only), FILE* crossing a user-defined function boundary (parameter or
  return — fired at the callee's first stream use, so a
  never-streaming FILE* parameter keeps its historical call-site
  rejection, pinned in fnptr-devirt-invalid.c), and FILE* anywhere but
  a function-local variable (struct member of a main-file record,
  global, array element). Non-handle FILE* shapes — `stdin`/`stdout`
  references, `FILE *g = stdout;` — keep the historical pointer
  machinery and its pinned wordings, and a bare `fopen(...)` statement
  keeps the system-header rejection.
  (test/Import/C/stdio-file.c, stdio-file-invalid.c,
  test/EndToEnd/stdio-file-roundtrip.c, 00187.c in the ledger)
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

Ledger as of 2026-07-18: 220 total / 217 passed / 0 miscompiled /
3 unsupported (was 150/70 at commit a091423, when this checklist was
drawn up; the quick wins, the CTS-S7/R5/P2/P4/P6 partials, and
CTS-S1/S2/S4/S6/P1/P5/P7/P8/R1/R2/R4/L1/L2 landed since, and the
2026-07-18 TDD wave took 200 -> 213: unions as one-slot structs
[+00042], void*-wildcard/member-base/null-ternary provenance [+00039
+00103 +00144 +00163], cell-slice global params + byte puns [+00181
+00217], fixed-prototype variadics + sprintf [+00140 +00186], the
StmtExpr pack [+00213 +00214], and fn-ptr devirtualization +
global-pointer returns [+00089 +00189]; the T1.1 ledger wave took
213 -> 215: dead-VLA elision + constant-LHS short-circuit folding,
byte-array union arms, and the local void* fn-ptr holder [+00207
+00210]; the T1.2 wave took 215 -> 216: C99-45 bit-field accessors
over backing runs + keyword-member mangling [+00218]; the T1.3 wave
took 216 -> 217: FILE* as an owned std::fs handle — fopen/fread/
fwrite/fgetc/fgets/fclose, the C99-48 stdio slice [+00187]). Every one
of the 3 is a located build-time rejection — never wrong output.
The 3 remaining are FINAL: 217/220 is this project's ceiling by
explicit decision (2026-07-18 survey, re-verified against the landed
T1.1-T1.3 machinery), not by backlog. Per-test dispositions:
- 00204 PERMANENT-OUT: the long-double diagnostic
  ("00204.c:36:28: error: unsupported builtin type 'long double'") is
  only the surface blocker — the fatal construct is a hand-rolled
  variadic reading `va_arg(ap, struct s7)` / `va_arg(ap, struct hfa34)`
  (struct-typed varargs / HFA calling convention), fundamentally
  outside the fixed-prototype variadic model and safe-Rust emission.
- 00209 UPHELD by-design rejection ("00209.c:24:10: error:
  unsupported: call with arguments through a function pointer without
  a prototype"): the K&R `int (*)()` call is ABI-unverifiable at
  import; overturning it would require callsite-prototype inference
  for a test whose main is `{return 0;}` and whose fn-ptr callers are
  never executed — near-zero value, declined.
- 00216 PERMANENT-OUT: beyond its first blocker (flexible array
  member, "00216.c:46:12: error: unsupported: flexible array member" —
  the C99-17 dedicated wording that replaced the generic
  "non-constant array size" fallback) it
  requires byte-exact struct layout INCLUDING padding (a print macro
  walks `(u8*)&x` over sizeof(x)), GCC range designators, and
  compound literals with relocations — byte-exact ABI layout is
  antithetical to the project's safe-Rust value model (the same reason
  bit-field layout is deliberately non-ABI, see C99-45).
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
- [x] CTS-P2 (7) Pointer types outside the parameter/local-cursor
  positions FR-28 classifies: pointer returns, pointer struct members
  (C99-43), pointers in casts and mixed expressions. Requires extending
  the region analysis beyond (base, cursor) pairs rooted in one
  function's locals. 00089 joined this set after CTS-L3 landed its
  fn_ptr-struct-field initializer: it now rejects on `struct S *anon()`
  at "00089.c:13:1: error: unsupported: pointer type outside a parameter
  position" (a data-pointer return).
  (00019.c, 00049.c, 00089.c, 00095.c, 00140.c, 00150.c, 00208.c,
  00214.c)
  (COMPLETE, 7 of 7 — 00089 landed last via the GLOBAL-RETURN kind
  (CTS-S stretch): a data-pointer-returning function whose every return
  site yields the address of ONE mutable whole global (cursor 0, never
  NULL) classifies as a single-global-base pointer RETURN region. The
  pointer result is ERASED from the imported signature — the function
  imports with no result and its return sites emit a bare `return` —
  the call is retained at each site for its side effects, and every
  caller `f()->member` access routes to the global directly through the
  ordinary staged-copy + writeback machinery: zero runtime pointer
  state (no cell, no flag, no address value) in callers. The erasure
  also flows through INDIRECT calls: a fn-ptr signature returning a
  data pointer is representable exactly when every address-taken
  function of that (canonical, unqualified) return type in the sole-TU
  program classifies to the erased kind with one common base
  (`classifyFnPtrPointerResult`), which covers 00089's
  `go()()->zerofunc()` chain — `go` returns `&anon` (the CTS-P2
  fn-address kind), `anon()` erases to global `s`, and the fn_ptr
  member call finishes through the CTS-L3 field machinery. Pinned OUT,
  located rejections: a NULL return site mixed with a global address
  ("return sites mix a global address and NULL"), disagreeing bases —
  including member-address sites rooted in different globals ("return
  sites disagree on the returned global base"), and callee-local
  returns keep the historical "unsupported: returned pointer value"
  dangling rejection.
  (test/Import/C/pointers-return-global.c,
  pointers-return-global-invalid.c; rustc-level differential
  test/EndToEnd/pointers-return-global.c; 00089.c in the ratchet
  manifest.)
  (Earlier partial state, 6 of 7 — 00019, 00049, 00095, 00140, 00150,
  00208 pass. Three
  sub-features landed, each the principal-kind inference of
  docs/transformation-theory.md sections 4-5 on the existing analysis.
  Pointer STRUCT MEMBERS: a data-pointer member is a stored i64 cursor
  field (a cursor is a borrow-free Copy integer, so a struct can hold
  one); the analysis resolves each member to one statically known target
  object — or one write-only string literal — per struct instance,
  program-wide (every body in Pass A plus the constant-initializer walk
  of globals, including a pointer global's compound-literal backing).
  Supported bindings are degenerate, so the stored i64 stays 0: member
  writes emit nothing, member reads resolve to the bound object's place
  with zero runtime state, and a self-referential chain folds hop by
  hop. Conflicting bindings are a located rejection naming both sites;
  aliased writes, escaping member addresses, and whole-struct overwrites
  poison the field program-wide. Pointer RETURNS: a data-pointer return
  type classifies by its return sites; the landed kind is a returned
  function address behind a void pointer (00095), emitted as the plain
  fn_ptr result — returning a cursor into a callee-local region stays
  rejected at the return site (the dangling case), and the caller-owned
  cursor-return kind (returning p+i over a slice parameter as a plain
  i64 the caller re-associates) is designed but not yet needed by any
  manifest test. CASTS: qualification-preserving explicit casts (same
  unqualified pointee) peel transparently in analysis and emission;
  reinterpreting casts stay rejected. 00140 passed once CTS-F1's
  fixed-prototype variadic definitions landed (its body never touches
  va_list; the pointer member and struct-by-value shapes already
  imported). 00214 passed once the CTS-P3 integer-carrier relaxation
  landed a second pointer-return kind: a function whose every return
  site yields a carrier value (a null constant, a pointer-width
  integer-to-pointer cast, a carrier-region local, or a call to another
  carrier-returning function) returns a plain i64 (see the CTS-P3 note),
  alongside its other blockers (__builtin_expect and StmtExpr, see
  CTS-S8). 00089 landed with the global-return kind above.
  Ledger 188 -> 193, zero miscompiles.
  (test/Import/C/pointers-member.c; test/Import/C/pointers-cast.c;
  member conflict/poison/literal-read/cross-function/dangling-return
  rejections in test/Import/C/pointers-member-invalid.c and
  test/Import/C/pointers-return.c; rustc-level differential
  test/EndToEnd/pointers-member.c)
- [x] CTS-P3 (5) Pointers assigned non-address values (integer↔pointer
  round-trips, arithmetic results stored back into pointers): the design
  decision landed as two relaxations rather than a tagged cursor — the
  CTS-P9 provenance core (void*-wildcard casts plus type-checked
  reinterpret-back sites) and the integer-carrier region model below —
  with every shape outside them a located by-design rejection pinned in
  the invalid tests. The null pointer constant is the None side of
  CTS-P8's Option-of-cursor; a pointer-width (64-bit) integer rides as
  a plain i64 carrier; every other non-address value (sub-pointer-width
  integer casts, carriers mixed with real address bases) stays
  rejected, each pinned in pointers-int-carrier-invalid.c. All five
  listed tests pass and are in the manifest.
  (00039.c, 00103.c, 00144.c, 00163.c, 00187.c)
  (Complete, 5 of 5 — the CTS-P9 provenance core. `void *` is a
  pointee-wildcard cursor: it carries no element unit of its own, so
  casts to and from a `void` pointee peel transparently in analysis and
  emission at any matching pointer depth (`(void *)&x`, `(int *)voidp`,
  the second-order `(int **)voidpp` of 00103), the (base, cursor)
  decomposition is unchanged, and a `void *` never materializes a
  pointer value. A reinterpret-back site `*(T *)p` type-checks T against
  the region's base element type: an exact match lowers exactly like a
  direct pointer (00039's scalar round-trip, 00103's double indirection),
  a same-width int<->int mismatch becomes an `emitrust.cast` bitcast
  view on the load and store (the unsigned view over int storage —
  Rust's same-width cross-sign `as` reinterprets the bit pattern, which
  is C's compatible-effective-type read), and every other
  reinterpretation stays a located rejection: "pointer cast reinterprets
  the pointee ('short' over 'int' storage)" / "('float' over 'int'
  storage)", plus "dereference of a 'void *' pointer" for an uncast
  deref. Null-only
  ternary chains (00144) fold statically — see the CTS-P8 note; the
  `&struct.member` bases of 00163 are the CTS-P7 note. 00039, 00103,
  00144, 00163 pass — ledger 200 -> 204 passed / 16 unsupported /
  0 miscompiled. 00187's actual blocker was FILE* streams, which
  landed as the C99-48 stdio slice (T1.3) — its handles never enter
  the pointer decomposition at all. Wide views over `char`
  storage (`*(unsigned *)charp`, from_ne_bytes territory) landed as
  CTS-P11.
  INTEGER-CARRIER REGIONS (the 00214 `extend_brk` brk-cursor shape)
  landed as a second relaxation: a local pointer whose ONLY sources are
  POINTER-WIDTH integer-to-pointer casts, calls returning carriers, and
  null pointer constants never addresses a modeled object — it is an
  integer riding in pointer clothing — and lowers as one plain i64
  value per pointer (null is the i64 zero; no base, cursor, or flag
  cell). A null test is an `arith.cmpi ne` against 0. The carrier
  crosses function boundaries in both directions: a pointer-returning
  function whose every return site yields a carrier returns a plain
  i64 (`classifyPointerReturn`'s second kind, memoized per canonical
  declaration with call-graph propagation), and a `void *` parameter of
  a defined function whose body only ever truth-tests it classifies as
  `ParamKind::Carrier` and imports as an i64 parameter (call sites pass
  carrier values). Pinned OUT, each a located rejection: "dereference
  of an integer-carrier pointer" (no object to read), "pointer
  arithmetic on an integer-carrier pointer" (no element run to walk),
  the historical "pointer assigned a non-address value" both for a
  carrier mixed with a real address base and for a sub-pointer-width
  integer cast (a truncated address can never round-trip; the MIXED
  and NARROW cases of pointers-int-carrier-invalid.c), and "void
  pointer parameter" for a `void *` parameter the body uses as
  anything but a truth test.
  Global pointers never carry integers (their facts feed the CTS-P4/P6
  machinery unchanged).
  (test/Import/C/pointers-void.c, pointers-void-invalid.c,
  pointers-int-carrier.c, pointers-int-carrier-invalid.c;
  rustc-level differential test/EndToEnd/pointers-void.c with
  loop-carried values through the reinterpreted accesses and
  test/EndToEnd/missing-return-expect-carrier.c; 00214.c in the
  ledger))
- [x] CTS-P4 (4) Pointer-typed global variables: global region bases.
  Landed as the single-global-region-base model below — the feared
  thread_local!+Cell interaction dissolved because a cursor is a
  borrow-free Copy integer, so no borrow ever escapes `.with`. C99-14
  now routes pointer-typed file-scope variables through this model
  (see its entry). Three of the four listed tests pass and are in the
  manifest; the fourth, 00209, clears its pointer-global blocker here
  and fails only on a C99-46-scope fn-pointer shape that is UPHELD as
  a permanent by-design rejection in the disposition list above.
  (00040.c, 00045.c, 00149.c, 00209.c)
  (Complete in scope, 3 of 4 passing: a pointer-typed global decomposes against a single
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
  0 miscompiled. 00209 stays out permanently, no longer on its pointer
  globals: after the pointer-to-fn-ptr parameter and fn_ptr slice
  extensions landed here (pointers-fnptr-slice.c), it rejects at
  "00209.c:24:10: error: unsupported: call with arguments through a
  function pointer without a prototype" — f1 calls through the K&R
  `int (*)()` typedef `fptr1`, a documented fn-pointer by-design
  rejection (C99-46 scope, not CTS-P4) UPHELD in the per-test
  disposition list above.
  (test/Import/C/globals-pointer.c, globals-pointer-invalid.c,
  pointers-fnptr-slice.c; rustc-level differential
  test/EndToEnd/pointers-global.c with data-dependent cursor updates
  across calls)
  (CTS-P10 interaction: the "passing a pointer into a global variable to
  a function" rejection is retired ONLY for the all-global cell-slice
  parameter class (see CTS-P6 below) — an argument mediated by a
  pointer-typed global (globals-pointer-invalid.c PASSFN) or by a local
  pointer into a global (pointers-local-invalid.c GLOBAL) keeps the
  staged-copy rejection verbatim, because those flows are outside the
  direct-decay/parameter-forwarding shapes the cell-slice class admits.)
- [x] CTS-P5 (2) Pointer-to-pointer values (`&p`, `**p`): second-order
  cursors over a region whose elements are themselves (base, cursor)
  pairs (C99-43). Implemented as the degenerate one-cell region of
  cursor cells: a `T **pp` bound (possibly repeatedly) to the address
  of exactly one first-order pointer local selects it statically, so
  `pp` carries no runtime state, `*pp` reads/rebinds the selected
  pointer's (base, cursor) decomposition (including its CTS-P8
  non-null flag), and `**pp` dereferences it — no
  reference-to-reference ever arises in the emitted Rust. Still
  rejected with located diagnostics: third-order pointers, pointers to
  function pointers, multi-target selections (would need a runtime
  second-order cursor), second-order copies, null second-order
  bindings, `&p` escaping outside a consumed `pp = &p` binding, and
  pointer-to-pointer parameters. 00005 and 00020 pass and are in the
  manifest — ledger 188 -> 190 passed / 30 unsupported /
  0 miscompiled.
  (test/Import/C/pointers-ptr-to-ptr.c, pointers-local-invalid.c;
  rustc-level differential test/EndToEnd/pointers-ptr-to-ptr.c with
  data-dependent re-pointing through `*pp`)
- [x] CTS-P6 (2) Pointers into global aggregates: same borrow-escape
  problem as CTS-P4; a global array base must be readable/writable
  through an index cursor without holding a borrow across statements.
  (00181.c, 00217.c)
  (Completed by the CTS-P10 cell-slice class and the CTS-P11 byte puns —
  see the resolution note after the CTS-P9 extension below. Ledger
  204 -> 206 passed / 14 unsupported / 0 miscompiled.)
  (Partial: a LOCAL pointer bound into a global aggregate (decay,
  `&garr[i]`, or `&gx`) now decomposes exactly like any Phase-1a local —
  its cursor stays a local i64 cell, so no borrow of the global is ever
  stored — and every element access through it goes through the CTS-P4
  staged-copy machinery: reads stage the global's whole value and
  subscript the copy; write contexts thread `GlobalWriteback` through
  `emitPointerPlace` and store the modified copy back, so a write
  through the pointer is visible to the next direct global access and
  vice versa (exact for the single-threaded subset; the historical
  one-statement last-writer-wins corner is closed by the writeback
  ordering rule below). Static-local
  bases get the same treatment (they share the globals map). Passing
  such a pointer to a function stays rejected at the call site — the
  argument would borrow the staged copy, not the global (the CTS-P4
  staged-copy coherence hazard) — as do escapes. Neither suite test
  passes yet, each on an out-of-scope shape: 00181.c rejects at
  "00181.c:117:4: error: unsupported: passing a pointer into a global
  variable to a function" (`Hanoi(N,A,B,C)` passes the global arrays
  into functions whose parameters also range over B and C — the staged-
  copy argument hazard plus CTS-P7 multi-object parameter regions);
  00217.c rejects at "00217.c:11:6: error: unsupported pointer
  expression: CStyleCastExpr" (`*(unsigned*)(data + r)` type-puns four
  chars of the global as an unsigned — a reinterpreting pointer cast,
  outside any CTS-P item). Ledger unchanged at 188 passed /
  32 unsupported / 0 miscompiled.
  (test/Import/C/pointers-into-global.c; staged-copy coherence-hazard
  rejection pinned in pointers-local-invalid.c GLOBAL case;
  rustc-level differential test/EndToEnd/pointers-into-global.c
  interleaving pointer writes with direct global reads, direct writes
  with pointer reads, and callee global writes between pointer uses)
  (Writeback ordering rule: the staged copy must be FRESH at store time.
  C11 6.5.16p3 sequences the RHS's side effects before the assignment's
  store, and the store writes only the designated subobject — so a
  whole-global snapshot loaded when the LHS place was formed must not be
  stored back after an intervening call wrote another subobject of the
  same global (the lost-update miscompile family). Every staged-global
  write path — simple and compound assignment, the CTS-P11 wide-byte
  stores, and ++/-- (including subscript-index calls, `g[f()]++`) —
  commits its mutation through the single `commitGlobalWriteback` seam
  in ImportC.cpp: when the statement evaluated any side-effecting
  subexpression after the staging load, the staged copy is rebound to a
  fresh `emitrust.global_load` snapshot immediately before the mutation,
  then flushed; pure statements emit the historical IR unchanged.
  Multi-base writebacks already re-stage afresh per dispatch arm inside
  the flush and need no refresh. User-visible evaluation order is
  untouched: every subexpression value is materialized before the
  refresh. Pinned differentially across all the write paths by
  test/EndToEnd/globals-writeback-order.c.)
  (CTS-P9 extension: `&global.member` is now a region base. The
  PointerBaseBinding carries an optional member path, and every access
  through such a pointer reuses this staged-copy machinery with a member
  projection — stage the whole global (`emitrust.global_load`), project
  the member (`emitrust.member`), and store the whole value back after a
  write (`emitrust.global_store`) — so no borrow of the global ever
  survives a statement. A member of a LOCAL struct resolves to the
  member's own `emitrust.member` place with no runtime state at all.
  Boundaries pinned by test: pointer arithmetic on a member base walks
  into sibling storage ("pointer arithmetic on the address of a struct
  member"), union storage has no unaliased member place ("taking the
  address of a union member"), and a member base on a pointer-typed
  GLOBAL stays rejected (the stored-cursor scheme has no member
  projection). 00163's `b = &(bolshevic.b)` exercises the global-member
  arm; test/Import/C/pointers-member-base.c and
  pointers-member-base-invalid.c pin the shapes, and the rustc-level
  differential test/EndToEnd/pointers-member-base.c rebinds the pointer
  at a data-dependent loop iteration.)
  (CTS-P10 resolution — cell-slice parameters, flips 00181: a pointer
  parameter whose interprocedural class is backed ONLY by mutable global
  arrays of one scalar element type now lowers to the shared
  `!emitrust.ref<!emitrust.cell_slice<T>>`, rendered
  `&[std::cell::Cell<T>]`. This is the coherence-sound choice the staged
  copy cannot make: 00181's Move mutates through its parameters and then
  calls PrintAll, which reads the SAME globals directly mid-call —
  `emitrust.cell_get`/`cell_set` and `global_load`/`global_store` hit
  the same thread-local Cell, so every write is observed. Pass A
  (`planCellSlices`) classifies via a union-find over exactly two
  argument shapes (direct global-array decay, parameter forwarding —
  which is what makes Hanoi's PERMUTED recursion classify: shared
  references are freely duplicable, no reborrow discipline). Call sites
  nest one `emitrust.global_cells` region per distinct global argument
  (leftmost outermost), rendered as nested thread-local `.with`
  accessors flattening through `as_slice_of_cells`, with a scalar result
  flowing out through a staging variable; element accesses in the callee
  are cell_get/cell_set on the reference itself (no lvalue staging, no
  cursor cell); forward prototypes classify through the definition
  exactly like Phase 1b. The historical "passing a pointer into a
  global variable to a function" rejection is retired for THIS class
  only; the pinned boundaries are located rejections with class-precise
  wordings: "pointer parameter would join global 'G' and local object
  'larr' into one region" (mixed classes stay out — one type cannot be
  both `&mut [T]` and `&[Cell<T>]`) and "nullable pointer parameter
  backed by a global variable" (Option wrapping and the global_cells
  borrow discipline do not compose in v1).
  Tests: test/Dialect/EmitRust/cell-slice.mlir (round-trip),
  test/Target/Rust/cell-slice.mlir (rendering),
  test/Import/C/pointers-global-args.c and
  pointers-global-args-invalid.c, and the rustc-level differential
  test/EndToEnd/pointers-global-args.c (mini-Hanoi: permuted recursive
  forwarding, direct global reads inside Move while cell borrows are
  live, a data-dependent disc count, and a Move return value flowing
  out of the .with nesting; greps pin as_slice_of_cells,
  &[std::cell::Cell<i32>], .with(, and the absence of unsafe).)
  (CTS-P11 resolution — byte puns over i8 regions, flips 00217: a
  wider-than-element reinterpreting deref `*(T *)p` over a region whose
  base element is a byte (a C char array) widens to a sizeof(T)-byte
  access at the runtime cursor: loads gather the bytes and combine with
  `T::from_ne_bytes`, stores split with `T::to_ne_bytes` and scatter
  back, and compound assignments read-modify-write the same window
  (00217's `*(unsigned*)(data + r) += a - b` with its wrapping u32
  delta). The direct pun cast `(unsigned *)(char *)...` is stripped only
  at dereference sites (`stripObjectPointerCasts`); the general
  decomposition still refuses to bind pointers through it. Global char
  arrays ride the ordinary staged-copy + writeback model, so a `%s`
  print of the global (extended to pointers into global char arrays)
  sees every punned byte. Boundaries pinned by test: a
  compile-time-constant offset whose window overruns the array rejects
  with "4-byte access at offset 5 runs past the end of 'buf' (8
  bytes)", and wide views over non-byte bases keep the existing
  "pointer cast reinterprets the pointee ('long long' over 'int'
  storage)" family (the deref type-check now also covers direct,
  non-void-mediated pun casts).
  Tests: test/Import/C/pointers-reinterpret.c and
  pointers-reinterpret-invalid.c, and the rustc-level differential
  test/EndToEnd/pointers-reinterpret.c (runtime offsets, partially
  overlapping wide stores, per-element/wide-view mixing, and the exact
  00217 shape over a global char array through a char* local; greps pin
  u32::from_ne_bytes, to_ne_bytes, and the absence of unsafe).)
- [x] CTS-P7 (2) One pointer ranging over several objects (`p = &x;
  ... p = &y;`): PointerRegionAnalysis unions the objects into one
  region today and rejects; needs either region materialization (copy
  both objects into one backing array) or an enum-of-bases cursor.
  (00077.c, 00172.c)
  (Done: the enum-of-bases cursor. A multi-base region is accepted when
  every base is local and of one uniform kind — all element runs (arrays
  or slice parameters) of the pointee's element type, or all degenerate
  scalars of the pointee type. Each pointer of the region carries a
  promotable rank-0 memref<i32> base-discriminant cell alongside its i64
  cursor cell (the tagged (base-index, cursor) pair; each variant of the
  closed enum names a disjoint region, so the disjoint-region invariant
  is preserved and the objects stay independently addressable —
  transformation-theory section 4, following the CTS-P8 flag-cell
  precedent). An address binding stores the bound base's index, `p = q`
  copies the source discriminant, cursor arithmetic is unchanged, and
  every dereference dispatches on the discriminant: a cf-level match
  over the closed set of bases whose arms touch exactly one base,
  staging the active element for reads and dispatching the mutated value
  back for writes (the staged-global writeback mechanism, generalized).
  Same-region equality compares (discriminant, cursor) pairs — exactly
  C's defined equality across distinct objects. Still rejected with
  located diagnostics: mixed base kinds and element types (the retained
  multibase.c negative), nullable multi-base regions, ordering and
  difference of multi-base pointers, passing one to a function or string
  helper, and non-scalar-element dereference. 00077 (param slice base +
  local array, sizeof forms) and 00172 (two scalars, equality before and
  after a discriminant copy) both pass; zero unsafe in the emitted Rust.
  test/Import/C/pointers-multi-base.c, the multi-* negatives in
  pointers-local-invalid.c; rustc-level differential
  test/EndToEnd/pointers-multi-base.c where the active base is
  data-dependent at runtime, including a loop-carried discriminant.)
  (CTS-P9 extension: the closed set of bases may now mix degenerate
  scalar locals with `&struct.member` bases, including members of
  GLOBAL structs (the 00163 shape, `b = &a; ... b = &bolshevic.b`).
  Member bases are degenerate one-element runs; a local-member arm of
  the dispatch touches the member's own place, and a global-member arm
  goes through the CTS-P6 staged copy with the member projection —
  stage the whole struct, project the member, store the whole value
  back on the write flush. Dispatch arms are ordered by base index
  (binding order) and the discriminant stays the promotable memref<i32>
  cell storing 0/1. test/Import/C/pointers-member-base.c @mixed_base;
  differential test/EndToEnd/pointers-member-base.c.)
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
  (CTS-P9 extension: a pointer-typed ConditionalOperator is now a
  pointer source — no new representation. Classifying `q = c ? A : B`
  classifies both arms into one united region (a null-constant arm,
  including the qualified `(const void *)0` spelling and the
  integer-conditional `q = i ? 0 : 0` shape, marks it nullable), and
  the emission assigns each arm in its own block so the null/address
  state merges through the pointer's own flag/discriminant/cursor cells
  (test/Import/C/pointers-null-ternary.c @ternary_real_base and the
  swapped-arm variant; differential test/EndToEnd/pointers-null-ternary.c
  with a loop-flipping condition). A base-less nullable region with a
  conditional source is STATICALLY NULL and carries zero runtime state:
  no flag cell is materialized, `if (q)` folds to a constant-false
  branch, `q == 0` folds true, `(int) q` folds to the integer 0 (the
  00144 ending), and dereference keeps the "only ever null" rejection
  (pointers-null-ternary-invalid.c; a pointer-to-int cast of a pointer
  with a real base keeps "unsupported cast (PointerToIntegral)"). A
  base-less region built only from DIRECT null bindings keeps the
  historical flag cell above — the pointers-null.c @null_only contract.
  00144 passes on this folding.)

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
  Partially landed (wave5 B1): the one-slot struct model (see C99-44)
  admits unions whose arms alias one leaf — identical mapped types or
  same-width integers differing only in signedness (bit-exact
  `emitrust.cast` reinterpretation at the accesses) — flipping 00042.c
  (untagged local two-int-arm union) to PASS in the manifest. 00210.c
  (packed/aligned char-array puns) and 00218.c (self-referential
  pointer-arm union) stay out of scope behind located
  `unsupported: union ...` rejections (test/Import/C/unions-invalid.c);
  positive pins in test/Import/C/unions.c and test/EndToEnd/unions.c.
  T1.1: the byte-array arm (00210's `uint16_t u; uint8_t b[2];`,
  packed attributes in either typedef position tolerated and discarded)
  now ADMITS at the TYPE level: the slot is the INTEGER arm regardless
  of declaration order, the array spelling never reaches the IR, and
  any access through the array arm is a located
  `unsupported: union byte-array arm access` at the ACCESS site;
  unequal-total-width array arms keep the union family rejection at the
  union decl. Together with the local void* fn-ptr holder (a
  never-reassigned local `void *` initialized from one known
  non-variadic function whose every value use is an explicit cast to
  exactly the target's signature in callee position imports as an
  ordinary `!emitrust.fn_ptr` local — fn-address `Some(target)`
  constant + `emitrust.call_indirect`, the cast fully peeled;
  out-of-shape holders keep `unsupported: pointer assigned a
  non-address value`), 00210.c flipped to PASS in the manifest.
  (test/Import/C/union-bytearray-arm.c, union-bytearray-arm-invalid.c,
  fnptr-void-local.c, fnptr-void-local-invalid.c,
  test/EndToEnd/fnptr-void-local.c) 00218.c stays out of scope.
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
- [x] CTS-R5 (3) C's separate tag/ordinary namespaces (`struct a` and a
  global `a` coexisting, or a static local colliding with the mangled
  `<fn>_<name>` scheme): Rust has one namespace per kind but the emitter
  uses one symbol table; resolved by detect-and-rename on collision.
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
  in the ratchet manifest; 00204.c clears its namespace blocker — the
  rename is complete for it too — and hits a construct outside this
  item's scope: "00204.c:36:28: error: unsupported builtin type 'long
  double'" (the C99-8 permanent rejection), behind which sit its
  struct-typed va_arg reads. 00204 is PERMANENT-OUT per the disposition
  list above, so nothing namespace-shaped remains: the item's scope is
  fully implemented and pinned, and its one non-passing test is
  dispositioned, not backlog.
  (00129.c, 00204.c, 00219.c)
- [x] CTS-R6 (1) Empty structs (`struct T {};` — a GNU/C2x shape clang
  accepts): emit a unit-like Rust struct.
  Empty-struct support landed: the importer accepts a field-less
  record, struct_def permits empty field arrays, and the emitter prints
  `struct T {}` (declaration/copy/default via the usual derives;
  test/Import/C/structs-empty.c, Dialect ops.mlir, Target memory.mlir).
  00216.c stays blocked on its next feature — the flexible array member
  `struct S s[];` rejects with "unsupported: flexible array member"
  (00216.c:46:12, the C99-17 dedicated wording) — so it remains off the
  manifest.
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
- [x] CTS-S5 (1) Variable-length arrays: conflicts with the
  deterministic/bounded design philosophy; recommend documenting as a
  permanent by-design rejection rather than implementing.
  Landed (T1.1) as DEAD-VLA ELISION, not VLA support: an UNREFERENCED
  local VLA whose size expression is side-effect-free is elided at
  import — no IR, no diagnostic; the object never materializes (the
  00207 f1 shape). Referenced VLAs, and dead VLAs whose size expression
  has side effects (eliding would silently drop the call), keep the
  verbatim `unsupported: non-constant array size` rejection. The same
  wave folds a compile-time-constant short-circuit LHS before lowering
  (`0 && printf(...)` / `1 || printf(...)` value shapes, mirroring the
  constant-condition ternary elision — the 00207 f3 shape), flipping
  00207.c to PASS in the manifest. General (referenced) VLAs remain a
  permanent by-design rejection.
  (test/Import/C/vla-dead-elision.c, vla-dead-elision-invalid.c)
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
- [x] CTS-S7 (2) `(void)` casts and void-typed contexts (evaluate and
  discard, `void` in a statement-expression position): map to an
  expression statement / `let _ =` discard; today "unsupported cast
  (ToVoid)" / "unsupported builtin type 'void'".
  Done for the manifest tests: ToVoid casts evaluate the operand as an
  expression statement (a side-effect-free operand emits nothing) and
  void-typed conditionals in statement position lower as if/else
  diamonds, which unlocked 00212.c; 00213.c passed once the CTS-S8
  StmtExpr pack landed (its label-containing constant-conditional arms
  keep full lowering — see the 00213 note under CTS-S8).
  (00212.c, 00213.c)
- [x] CTS-S8 (2) The StmtExpr pack (the 00213/00214 shapes): GNU
  statement expressions, `__builtin_expect`, constant-condition dead-arm
  elision, and fall-off-the-end return synthesis.
  STATEMENT EXPRESSIONS `({ ... })` lower as FLATTENED statements in the
  enclosing function — never as a walled-off region op (an
  `scf.execute_region` would hide internal labels from the
  labelBlocks/goto dispatch) — with the final expression statement's
  value transiting a synthesized temp cell (memref for signless scalars,
  `emitrust.variable` for unsigned) that the surrounding expression
  reads; nested StmtExprs flatten recursively, labels inside register
  with the ordinary goto machinery (the within-StmtExpr backward-goto
  loop lifts to `scf.while`), and a statement-position StmtExpr
  discards its value (a side-effect-free final expression emits
  nothing). Pinned OUT: "goto out of a statement expression in value
  position" (the value temp would never be written); a StmtExpr whose
  last statement is not an expression is ill-formed C in value position
  and clang itself rejects it.
  __BUILTIN_EXPECT (and the _with_probability form) is a pure
  branch-prediction hint: it folds to its first argument at the emitCall
  seam in every position, so no call op or `__builtin_expect` symbol
  survives into the IR, and a constant argument composes with dead-arm
  elision through clang's constant evaluator.
  CONSTANT-CONDITION DEAD-ARM ELISION: an `if`, value ternary, or void
  ternary whose condition constant-folds (side-effect-free) elides the
  dead arm BEFORE lowering — before any unimported-call or conversion
  check, so a dead arm may contain otherwise-unimportable constructs
  (00214's `if (__builtin_expect(!!(0), 0))` arms and `_Bool chk`).
  Gated on a live-label check applied uniformly to the if, value
  ternary, and void ternary forms: a dead arm holding a goto-targeted
  label keeps FULL lowering — the constant branch leaves the arm
  dynamically dead while its labels register with the ordinary goto
  dispatch, so code entered through the label runs exactly as C
  requires (the 00213 `if (0) { lab: ... }` and kb_wait_1 shapes). This
  is sound for ternary arms too: a label there can only live inside a
  statement expression, clang rejects any jump INTO a statement
  expression from outside, and the flattened StmtExpr lowering (CTS-S8
  above) registers internal labels like any others, so full lowering
  needs no jump-around suppression. An arm holding a case/default label
  of an enclosing switch is likewise never elided (full lowering
  through the existing dispatch-switch machinery — silently dropping it
  would miscompile).
  MISSING-RETURN SYNTHESIS: a non-void function whose control falls off
  the end (C11 6.9.1p12 — defined while the caller never uses the
  value) synthesizes `return 0` of the function's return type at
  finalization, for every integer width including `_Bool`/i1 and for
  floats; aggregate/enum/fn_ptr returns keep the located rejection.
  00213 CAPTURED: its kb_wait_1 constant void-ternary holds a
  goto-targeted label inside the DEAD StmtExpr arm, targeted from
  within that same arm — exactly the full-lowering-instead-of-elision
  case above. The label-containing arm lowers fully behind the constant
  branch, the internal backward goto resolves through labelBlocks, and
  no code suppression is needed, so the composed lowering is exact and
  00213 joins the manifest alongside 00214.
  (test/Import/C/stmt-expr.c, stmt-expr-invalid.c, builtin-expect.c,
  missing-return.c; rustc-level differential
  test/EndToEnd/stmt-expr.c and
  test/EndToEnd/missing-return-expect-carrier.c; 00213.c and 00214.c
  in the ledger)

### Functions and linkage (2 tests)

- [x] CTS-F1 (2) Variadic calls and variadic function-pointer types
  beyond the printf/puts intrinsics (C99-37): design decision needed
  (safe Rust has no C-style varargs; candidates are arity-specialized
  monomorphization at call sites, or rejection).
  (00186.c, 00189.c)
  (COMPLETE, 2 of 2 — 00189 landed last via STATIC DEVIRTUALIZATION
  (CTS-S stretch): a file-scope function pointer initialized to a known
  function and NEVER REASSIGNED (nor address-taken) anywhere in the TU
  — the criterion is never-reassigned, not const-qualified; an
  externally visible variable only qualifies in a sole-TU import — is
  an import-time ALIAS of its target. No `emitrust.global` is
  materialized for it, calls through the alias (both `p(...)` and
  `(*p)(...)`) lower as DIRECT calls to the target after the same
  signature check a `Some(target)` constant runs (no fn_ptr value, no
  call_indirect), and a value use reads as the `Some(target)` constant.
  A variadic target aliases only when it is the hosted definition-less
  printf/fprintf: calls route through the printf machinery, and the
  fprintf shape swallows its leading `stdout` argument with the
  fprintf->printf routing — the swallowed first-arg slot is the ONLY
  place a FILE* value is accepted (00189's
  `fprintfptr(stdout, "%d\n", (*f)(24))` composition). Pinned OUT,
  located rejections: a reassigned global fn-ptr and a never-reassigned
  pointer to a NON-hosted external variadic keep the ordinary import
  path's "unsupported: variadic function pointer type" at the decl;
  `stdout` outside the swallowed slot keeps "unsupported: pointer
  variable 'stdout' has no known target object" at the use; storing
  `stdout` keeps "unsupported: copying a global pointer variable".
  (test/Import/C/fnptr-devirt.c, fnptr-devirt-invalid.c; rustc-level
  differential test/EndToEnd/fnptr-devirt.c; 00189.c in the ratchet
  manifest.)
  (Earlier partial state, 1 of 2 — 00186 passes. Two sub-features landed. VARIADIC
  DEFINITIONS whose bodies never touch va_list (no va_start/va_arg/
  va_copy calls, no va_list declarations) import as their FIXED
  prototype — the named parameters only, the trailing `...` dropped
  from the type; call sites drop trailing extras when every dropped
  extra is side-effect-free (the dropped extras are never imported:
  no loads of by-value struct extras, no borrows for dropped `&s`,
  no pointer regions). Pinned OUT: a definition whose body uses
  va_list keeps "unsupported: variadic function definition", and a
  dropped extra with side effects rejects with "unsupported: extra
  argument to a variadic call has side effects" at the call site;
  variadic function-pointer TYPES stay rejected — 00189's fprintf
  pointer later became representable without the type, via the
  devirtualization alias above. This also unblocked 00140 (see
  CTS-P2). SPRINTF with a
  literal format (00186) lowers through the printf-shared directive
  translator into a `format!` String plus the one-per-module safe
  `__emitrust_sprintf(dest: &mut [i8], s: &str) -> i32` helper
  (bytes + NUL copied via bounds-checked indexing — a too-small
  destination panics, a legal refinement of C's UB — returning the
  length), with the destination borrowed mutably from its cursor like
  the <string.h> helpers. Pinned OUT: non-literal formats
  ("unsupported: sprintf format must be an ordinary string literal"),
  precision (shared translator's "unsupported: precision in printf
  format specifier"), and string-literal destinations ("unsupported: a
  string literal region cannot be a mutable string argument").
  Ledger 201 -> 203 (+00140 +00186), zero miscompiles.
  (test/Import/C/varargs-def.c, varargs-def-invalid.c, sprintf.c,
  sprintf-invalid.c; rustc-level differentials
  test/EndToEnd/varargs-def.c, test/EndToEnd/sprintf.c))

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
- [x] CTS-L3 (2) String-literal and other initializers for
  pointer-typed objects (`char *s = "…"` at file scope, struct fields):
  COMPLETE — the initializer shapes landed, 00220 passes, and 00089
  passes since CTS-P2's global-return kind landed (its own blocker; the
  initializer shape it needed is item (3) below). Landed: (1) a
  file-scope `char *s = "…"` imports in
  importPointerGlobal as the CTS-P1 read-only backing lifted to module
  scope — a const `<name>_backing` byte-array global (bytes plus NUL,
  ASCII-only per C99-28) plus the CTS-P4 stored i64 cursor global,
  offset-initialized; write-through, null, wide/u8 literals, non-ASCII
  bytes, literal/object joins, and body literal bindings keep located
  rejections (write-through detection now tracks a global pointer whose
  only body mention is the write); (2) wide-literal array initializers
  (`wchar_t s[] = L"…"`, block and file scope) fill i32 arrays with the
  literal's code units, no ASCII limit (a wide array never feeds the
  byte-string `%s`/`%c` helpers), which is all 00220 needs — it passes
  end-to-end (ledger 188 -> 189); (3) fn_ptr struct fields in file-scope
  initializers (the 00089 line-10 shape) fold to the opaque
  `Some(name)`/`None` forms via the constant evaluator, verifier and
  emitter accept them as aggregate leaves. 00089's last blocker —
  `struct S *anon()` returns a data pointer — landed as CTS-P2's
  global-return kind, and 00089 now passes end-to-end.
  (test/Import/C/globals-pointer-string.c, strings-wide.c,
  fn-pointers.c; rejections in globals-pointer-invalid.c,
  strings-invalid.c; rustc-level differential
  test/EndToEnd/globals-string.c)
  (00089.c and 00220.c pass)
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
goto/labels are supported per C99-33), unions outside the one-slot
model (CTS-R3), bit-fields outside the C99-45 backing-run accessor
model,
int-to-enum conversions, pointer-to-pointer values, pointer struct fields,
NULL data pointers, void* casts, malloc and friends
(pointer arithmetic, pointer locals, and pointer/array parameters are now
supported through the FR-28 decomposition; pointer-typed globals with one
global region base are supported per CTS-P4, including its carve-out
promoting a single constant-size calloc/malloc site bound to a global
pointer into a static backing array),
multi-dimensional arrays, sizeof/_Alignof of
variable-length-array/incomplete/function operands, conditional operators
with non-scalar results, va_list-using variadic definitions (a variadic
definition whose body never touches va_list imports as its fixed
prototype per CTS-F1, with effect-free trailing extras dropped at call
sites), and `char *` variables
bound to string literals (aggregate initializer lists are supported per
C99-11/12, `char s[] = "..."` and the printf/puts %s shapes per
C99-28/47). These are natural follow-ons; the emitter's
statement-per-op model is chosen precisely so expression inlining can be
layered in later, as EmitC did.

## RFC: Actor-model emission — deferred

Status: design RFC only. No ops, passes, analyses, or tests for actor
emission exist in the tree, and this cycle adds none. FR-30 shipped the
project's only "actor" — the owner struct, an *ownership boundary, not a
thread*. This RFC examines the threaded reading (actors as spawned
threads with mailboxes) and concludes it must stay deferred; the
go/no-go criterion at the end is the falsifiable condition for ever
reopening it. Throughout, claims about existing machinery cite the
actual code; everything labeled "sketch" is speculative and unbuilt.

### Input subset where actor decomposition is semantically justified

The candidate subset is: programs whose entry point performs two or more
independent top-level work items — call trees that share no mutable
state, meaning (1) the pointer-region classes reachable from each
candidate's call tree are pairwise disjoint, and (2) the candidates
share no mutable globals, whether reached through pointers or accessed
directly by name. The certifying analysis would be the Pass-A
interprocedural machinery: `planOwners` (lib/ImportC/ImportC.cpp)
already builds a program-wide (per-TU) union-find over storage bases and
data-pointer parameters on the shared `VarDeclUnionFind` scaffold,
adding one edge per data-pointer call argument via the
`forEachDataPointerCallArg` call-edge walk, and `planCellSlices` reuses
the same scaffold for global-array-backed parameter classes. Two
candidate actors would be independent iff their reachable region classes
are disjoint and their mutable-global footprints do not intersect.

Honest assessment: **this claim is NOT dischargeable by today's Pass-A
machinery.** Three gaps, verified against the code as of this writing.
First, there is no call graph: `forEachDataPointerCallArg` visits call
expressions only to add pointer-argument union edges; nothing computes
the transitive callee closure from a candidate root, which is the very
object whose footprint must be certified. Second, the union-find models
*pointer-carried* state only — a global scalar or array read and written
directly by name (the `global_load`/`global_store` path) never enters
the analysis at all; `globalPtrFacts` merges region views of
pointer-typed globals, not the by-name access footprint of ordinary
globals, so the "shares no mutable globals" half of the independence
condition has no computer today. Third, hosted-library effects are
outside the region model entirely: `printf`/`putchar` mutate the one
resource — stdout — that essentially every corpus program touches, and
no analysis attributes output effects to functions. What is genuinely
reusable is the scaffold: the per-body `PointerRegionAnalysis`, the
`VarDeclUnionFind` fixpoint, and the Pass-A "walk every definition,
merge program-wide facts" pattern would host a new effect-footprint
planner (per-function: mutable globals touched by name, region classes
touched, output effects; then a transitive closure over a real call
graph). That planner is new work, not an incremental tweak, and its
absence tightens the go/no-go criterion below.

### Dialect surface sketch (sketch only — no code this cycle)

The ops would follow the `emitrust.global_cells` model: region-based
statement ops with verifier-pinned structure, so no handle or borrow
ever escapes into general SSA circulation.

`actor.spawn` — sketch. A statement op carrying one FlatSymbolRefAttr
naming the actor's state definition (an FR-30 owner struct or a
dedicated actor `struct_def`), no operands and no results, with a single
sized region whose entry block takes exactly one argument: an opaque
actor-reference value typed against the named definition. Like
`global_cells`, the actor lives exactly for the region's extent — the
region's end is the join point where the mailbox sender drops and the
actor thread is joined. Verifier obligations: the symbol resolves to an
actor definition (SymbolUserOpInterface), the region is single-block
with the one correctly typed entry argument, the terminator is
`emitrust.yield`, and the reference argument is used only as the target
operand of `actor.send`/`actor.call` ops nested inside the region.

`actor.send` — sketch. A statement op taking the actor reference, a
message-tag attribute naming a variant of the actor's declared message
enum, and one value operand per payload field. No results. Verifier
obligations: the op is nested inside the `actor.spawn` region that
introduced the reference; the tag names a declared variant; each payload
operand's type equals the variant's declared field type and lies in the
sendable value set (see soundness below) — never an lvalue, reference,
or cell-slice type.

`actor.call` — sketch. Operands exactly as `actor.send`, plus one
result whose type equals the tagged variant's declared reply type.
Verifier obligations: everything `actor.send` requires, plus the reply
type match, plus the structural nesting guarantee that the call cannot
outlive the spawn region (so the blocking receive always has a live
counterparty and the reply channel cannot dangle).

### Lowering: std::sync::mpsc loops only

The lowering posture is std-only — `std::thread` plus
`std::sync::mpsc` — consistent with the zero-dependency emitted crates
(no tokio, no actix; rejection recorded below). Sketch of the emitted
shape, per actor: one message enum with one variant per handled message
(fields are the payload values; call-style variants carry an extra
`mpsc::Sender` for the reply); one `mpsc::channel` pair created at the
spawn point; one `std::thread::spawn` whose move closure takes ownership
of the actor's state struct and the receiver and runs the mailbox loop —
a `for`-over-receiver whose body is a single `match` on the message
enum, each arm invoking the corresponding `&mut self` method on the
owned state, call-style arms sending the method result back on the
carried reply sender. The loop terminates when the last sender drops at
the end of the spawn region, after which the spawn's join completes.
`actor.call` lowers to a send of the variant carrying a fresh reply
channel's sender, immediately followed by a blocking receive on the
reply receiver; a disconnected reply channel is a deterministic panic,
consistent with the project's policy of refining UB and impossible
states into deterministic panics rather than unsafe.

### Soundness obligations mapped to existing machinery

No shared mutable state across actors: exactly the region-disjointness
plus global-footprint condition of the subset section — dischargeable
only after the Pass-A effect-footprint extension described there. One
project-specific landmine makes this obligation stricter than in an
ordinary Rust codebase: emitted globals are `thread_local!` + Cell, so a
spawned actor thread observes *fresh, reinitialized* globals, not the
main thread's — any actor whose closure touches any global is silently
wrong today, and the certification must therefore prove a zero-global
footprint (or the global model must be reworked), not merely a
non-conflicting one.

Message payloads are Copy/owned values: the existing value model already
supplies the sendable set — scalar ints/floats/bools, C-like enums, and
derive-Default Copy structs are plain values. Two existing value kinds
must be *excluded* despite being Copy: region cursors (a cursor is a
plain i64 only meaningful against its region's base — sending one across
an actor boundary would smuggle aliasing into the receiver) and function
pointers whose targets' footprints fall outside the receiving actor's
certified partition.

Deterministic output interleaving: not an obligation existing machinery
can discharge at all; it is the hard constraint of the next section, and
under it at most one actor per program may produce output, with all
cross-actor interaction synchronous.

### Non-goals and the hard constraint

The project's oracle is byte-identical stdout and exit status against
the natively compiled C program — enforced by every EndToEnd
differential test, by the c-testsuite ledger in CI, and by the three-way
fuzz contract (genprog's Python evaluator, clang, and emitrust-cc must
all agree byte-for-byte). That oracle encodes *sequential* semantics.
Any concurrency the emitter introduces must therefore be observationally
sequential, and that requirement hollows out the actor model:

- Synchronous request-response (`actor.call` = send + blocking receive)
  is the only shape that preserves sequential observation — and under
  it, the mailbox is pure ceremony. The run loop is a function call with
  extra steps: strictly more machinery, an extra thread, and identical
  observable behavior to the FR-30 method call that already exists.
  This RFC states that plainly rather than dressing it up.
- Mutual-recursion SCCs collapse into one actor. A synchronous call
  from actor A blocked on actor B that calls back into A deadlocks a
  single-mailbox actor, so every call-graph SCC (the Hanoi shape —
  CTS-P10's permuted mutual recursion — is the canonical corpus example)
  must be assigned to a single actor. Hanoi-shaped programs therefore
  decompose into exactly one actor: zero benefit.
- Genuinely concurrent actors (asynchronous sends, interleaved
  progress) produce nondeterministically interleaved output and would
  need an ordering-insensitive oracle — per-actor output streams, or
  sorted/multiset output comparison — which the project deliberately
  does not have and will not build for this: the byte-exact three-way
  contract is the project's core defense against miscompiles, and
  weakening it to enable a feature with no demonstrated demand inverts
  the project's priorities.

Recorded rejections. tokio/actix: the emitted crates are
zero-dependency by contract (plain cargo build, no network, no
third-party audit surface), an async runtime adds a large dependency
tree and scheduler-dependent execution order, and the RFC's std-only
posture already covers the only admissible shape. Deep-copy-across-
boundary: the cell-slice wave (CTS-P10) proved staged copies unsound for
interleaved global readers — 00181's Move mutates through its
parameters while PrintAll reads the SAME globals directly mid-call, so
only the shared-Cell lowering is coherent. An actor design that
deep-copies shared state into messages and writes it back on reply is
that same staged copy wearing a costume: any reader interleaved between
the copy and the write-back observes stale state. Sharing must be
compiled away by the independence certification, never papered over by
copying.

### Go/no-go criterion

Concrete and falsifiable, tightened by the honest finding above that
the independence certification is not dischargeable by today's Pass A.
Implement actor emission only when ALL of the following hold:

1. A Pass-A effect-footprint planner exists (call-graph transitive
   closure; per-function by-name mutable-global footprint; output-effect
   footprint; merged with the existing region classes) — this is a
   prerequisite build, not a given — AND, run over a target corpus of at
   least 10 real programs (c-testsuite members or user-supplied), it
   certifies in each program at least 2 independent top-level work items
   with pairwise-disjoint region classes, zero shared mutable globals,
   zero per-actor global footprint under the thread_local! model (or a
   reworked global model), and at most one output-producing actor.
2. Each certified program's output is provably order-independent or
   fully serialized through the single output-producing actor, so the
   byte-exact oracle still applies unchanged.
3. A differential oracle for any reordered output exists and is wired
   into the three-way fuzz contract without weakening the byte-exact
   comparison for the sequential corpus.

Until all three hold simultaneously, this RFC stays deferred, and the
FR-30 owner struct remains the shipped meaning of "actor". Criterion 3
is expected to remain unmet indefinitely by deliberate choice; the RFC
exists to record why, not to schedule the work.

## Validation

Every requirement above is validated exclusively by regression tests run by
the lit suite; no requirement is checked off based on manual inspection.
The suite must pass warning-free in the pinned dev shell before a
requirement's box may be ticked. CI (.github/workflows/ci.yml) enforces the
same gate on every push and pull request: the full check-emitrust suite in
the pinned Nix dev shell, plus an explicit c-testsuite conformance step in
which every test listed in test/CTestSuite/expected-pass.txt is required to
pass differentially and any MISCOMPILE or ratchet violation fails the run.
