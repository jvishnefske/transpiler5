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
  supported and value-preserving (the open-enum representation holds any
  value of the underlying type, matching the C99-5 enum section and CTS-S6).
  (test/Import/C/enums.c, enums-invalid.c, enum-from-int.c,
  test/Target/Rust/match.mlir, test/EndToEnd/switch-enum.c,
  test/EndToEnd/enum-from-int.c, enum-int.c)
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
  arms accessed only through the integer arm, dead-VLA elision
  noise, C99-13 compound literals (loop re-zero rule pinned), C99-4/28
  signed-char/escape/string-literal-expression semantics, the C99-48
  curated libc subset, C99-44 float puns observed via bits only, the
  C99-47 printf format-language growth (float formats only over a
  Python-glibc byte-verified curated pool), and inline/array-param/const
  qualifier noise, each modeled on a test/EndToEnd differential test — into UB-free C11 programs whose
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
    global_load, method_call, emitrust.bitcast (a float bit pattern is
    honestly the full integer range), and values inside emitrust region
    ops whose lattices stay uninitialized — all read as TOP at
    observation points.

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
  K&R callsite-prototype inference (CTS 00209): an argument-carrying
  call whose callee traces (after the `(*fp)` deref-peel) to a
  local-storage parameter/local of prototype-less pointer type refines
  that decl to `fn_ptr<promoted... -> ret>` inferred from the call's
  default-promoted argument types; agreeing sites share the refinement,
  a disagreeing site is a located conflict rejection, and never-argument-
  called no-proto decls keep the unrefined zero-parameter mapping.
  Located rejections: variadic targets, signature mismatches (including
  prototype-less K&R pointers bound to functions incompatible with the
  inferred or zero-parameter signature),
  argument-carrying calls through prototype-less values NOT traceable
  to an inferred decl (members, array elements, call results), fn_ptr
  component types outside the supported set (e.g. data-pointer
  parameters), and arrays of function pointers. The differential test is
  byte-identical to clang and the emitted crate contains no unsafe.
  (test/Import/C/fn-pointers.c, fn-pointers-invalid.c,
  fnptr-noproto-infer.c, fnptr-noproto-infer-invalid.c,
  test/Target/Rust/fn-pointers.mlir, test/Dialect/EmitRust/types.mlir,
  ops.mlir, invalid.mlir, test/EndToEnd/fn-pointers.c,
  fnptr-noproto-infer.c; c-testsuite 00087, 00088, 00124, 00209)
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
- [x] FR-31 Whole-program cross-TU analysis substrate, and two closed-
  program correctness fixes (W3.2). `planOwners`/`planCellSlices`/
  `planFnPtrAliases` (FR-30/FR-28's Pass A) do ZERO cross-TU merging: each
  runs per TU over fresh state keyed by `clang::Decl*`, which has no
  identity across the independent `clang::ASTContext` instances
  `importCProject`'s per-file `ClangTool` parses (W2.0). `WholeProgramInfo`
  (CImporterInternal.h) is a symbol-name-keyed whole-program fact base —
  MLIR symbol name is the one identity that bridges the independent
  ASTContexts, the same bridge W3.0's `crossTuVaListVariadicNames` already
  uses — populated by `collectWholeProgramInfo`, a pre-import pass run
  over EVERY parsed AST (swapping `astContextPtr` per AST, mirroring
  `collectCrossTuVaListVariadics`) BEFORE `importCProject`'s per-TU import
  loop begins, so every fact is complete regardless of which TU is
  processed first. This relies on the CLOSED-PROGRAM ASSUMPTION already
  implicit in `importCProject`'s contract: it receives every translation
  unit of the project up front and keeps every parsed `ASTUnit` alive for
  the whole call, so a `clang::Decl*`/`ASTContext&` from any TU stays a
  valid, comparable identity for the entire import, even one from a TU
  that has not been (or will never be) the "current" one. Landing this
  substrate alone changed nothing (verified byte-identical importer output
  over the whole Import/EndToEnd corpus): nothing consumed it yet. Two
  fixes now consume it, both scoped to the narrow, verifiably sound shape
  each needs and falling back to the historical rejection for every other
  shape:
  - Shared pointer global: `extern int *g;` in one TU, `int *g = &arr[0];`
    defined in another (the natural header-shared pointer-global idiom).
    This is NOT the `importPointerGlobal` external-linkage gate (which
    only fires when a TU both defines AND locally uses its own pointer
    global) — the defining TU here never locally references `g` (only
    initializes it), so `importPointerGlobal`'s referenced-only skip fires
    and that gate never runs; the actual rejection was a separate,
    unconditional check in `deferExternGlobal` refusing every pointer-typed
    extern outright. The whole-program pre-scan evaluates every file-scope
    pointer global's initializer AND every function-body reassignment
    project-wide (the same `PointerRegionAnalysis`/`mergeRegionFacts`
    machinery `planOwners` uses, merged by symbol name instead of per-TU
    `Decl*`), recording a symbol as reconstructible only when the whole
    project shows EXACTLY one base, bound by a plain file-scope
    initializer, with NO reassignment anywhere (a dynamically reassigned
    pointer's cursor is a runtime value with no compile-time offset — out
    of scope here, and still rejected). For such a symbol,
    `deferExternGlobal` delegates to `deferExternPointerGlobal`, which
    EAGERLY re-imports the base object from its OWN TU's `ASTContext`
    (idempotent — `importGlobalVar` already guards re-entry — and order-
    independent, since the base's `Decl*` and its owning `ASTContext`
    remain valid for the whole `importCProject` call) and synthesizes the
    extern's own `i64` cursor global, comparing the base's element type
    against the pointee by their MAPPED MLIR types (not the raw clang
    `QualType`s, which are interned per-`ASTContext` and would spuriously
    disagree across TUs even for identical C types). A divergent rebinding
    (the same externally visible pointer bound to two different bases
    across TUs) keeps rejecting: the single-base cursor model has no
    representation for "the base depends on which TU last ran."
  - Incomplete-extern-array composite merge: `extern int a[];` in one TU,
    completed by `int a[4] = {...};` in another (C99 6.2.7). Each TU parses
    as an independent `ASTUnit` with no cross-TU type composition step, so
    the incomplete declaration's OWN (incomplete) type could never map —
    a real gap, not a conservative gate (the bounded case, `extern int
    a[4];`, already worked). The whole-program pre-scan records, for every
    externally visible bounded-array DEFINITION in any TU, its complete
    element type — mapped with `mapSpeculativeArrayType`, a small
    memoization-free mirror of `mapType`'s plain-scalar-builtin cases ONLY.
    This deliberately narrower mapper is required, not a shortcut: `mapType`
    itself consults `isByteRegionRecord`/`isByteRegionAggregate`, whose
    memoized classification is sound only once THIS TU's own Pass-A
    (`collectDeclTypeRecords`/`planFnPtrMembers`) has run; calling the real
    `mapType` speculatively, before any TU's Pass-A runs, was tried first
    and poisoned that cache for the real import that followed (caught as a
    c-testsuite ledger regression on 00216, a `void*` fn-ptr struct member
    classified inconsistently — a reminder that even a "purely additive,
    nothing consumes it" fact-gathering pass can still corrupt a later
    pass through shared mutable memoization state, independent of whether
    its own diagnostics are suppressed). `deferExternGlobal` resolves an
    incomplete-array extern's type from this whole-program fact when a
    completing definition exists project-wide, exactly mirroring how the
    EXISTENCE check is already deferred via `pendingExternGlobals`; absent
    a completing definition, or for any element type the narrow mapper
    does not handle, it keeps the historical "non-constant array size"
    rejection.
  Both fixes are order-independent (verified with the TU list given in
  both orders) and produce byte-identical crate output vs the clang-linked
  native binary. Pass-A planners (`planOwners`, `planCellSlices`,
  `planFnPtrAliases`, `planCursorParams`, `planVaMonomorph`,
  `collectDeclTypeRecords`, `planFnPtrMembers`) moved from ImportC.cpp to
  ImportC/ImportCPlanning.cpp by pure code motion in the same wave (they
  were deliberately left behind during the W1.8-W1.12 file split for
  exactly this purpose); `collectWholeProgramInfo` and
  `collectCrossTuVaListVariadics` (W3.0) stay in ImportC.cpp since they are
  whole-program passes, not per-TU planners.
  (test/Import/C/multi-tu-gate-g8-ptr-global-shared-header.c,
  multi-tu-gate-g8-ptr-global-negative.c (must stay rejected),
  multi-tu-gate-g8-ptr-global-external.c (G8 itself, untouched, must stay
  rejected), multi-tu-extern-array-composite-merge-probe.c;
  test/EndToEnd/multi-tu-gate-g8-shared-header.c)

- [x] FR-32 Cross-TU cell-slice region APIs (W3.3 G4/G5/G6). `planCellSlices`
  (CTS-P10) ran per TU and blanket-excluded every externally visible
  function parameter (a preemptive poison), global-array base, and owning
  function from the cell-slice path, because an unseen TU could call the
  function with a Cell-less local argument. The EMISSION model is already
  generic — a cell-slice parameter is `&[Cell<T>]` with the concrete global
  bound per call site via `emitrust.global_cells`, so the same function can
  back a different global at each call site — so lifting these gates is
  confined to planning, needing no IR change. A whole-program pre-pass
  (`collectCellSliceCallFacts` per TU + `finalizeCellSliceWholeProgram`,
  riding the same pre-import loop as `collectWholeProgramInfo`) records, for
  each externally visible function's data-pointer parameter (keyed
  `"<fnSymbol>#<index>"`), the externally visible globals passed to it
  project-wide and whether any call passes a Cell-less argument (a local
  decay, an interior pointer, a pointer-to-pointer parameter, or a forwarded
  parameter — the last conservatively poisoned, an untested cross-TU shape).
  A parameter is cell-slice-eligible when it is not poisoned AND backed by at
  most ONE externally visible global — the whole-program form of the
  single-TU multi-base disqualification (two distinct external globals
  through one parameter is unrepresentable in the single-base cursor model,
  g5 negative). INTERNAL globals are never merged across TUs, so one external
  function may back a different internal global in each TU (g6: `sum4(A)` in
  one TU, `sum4(B)` in another, each via its own `global_cells` region).
  `planCellSlices` consults these eligibility sets to lift its three
  external-linkage exclusions per parameter/global; every other per-TU check
  (body escape, null-check, element type, mutable array) still fires, so
  eligibility only ever ADDS the "not the historical blanket external
  reject" permission — it never forces a shape the per-TU analysis rejects.
  ORDERING LIMITATION (sound, documented): the promotion needs the callee's
  cell-slice signature established when a call site is emitted, which happens
  where the DEFINITION is imported; a call site in a TU processed BEFORE the
  defining TU conservatively falls back to the historical rejection (a missed
  optimization, never a miscompile). Order-independent cell-slice signatures
  (driving the signature from the whole-program eligibility) are a future
  refinement, gated on the pre-pass also replicating the definition-side
  body/type checks so an eligible signature always implies a cell-slice-able
  definition. Legitimate only under the same CLOSED-PROGRAM ASSUMPTION as
  FR-31 (`importCProject` sees every TU). Byte-identical crate-vs-native
  differentials; the three negatives (local argument, multi-base external
  globals) stay rejected.
  (test/Import/C/multi-tu-gate-g4-cellslice-poison.c,
  multi-tu-gate-g4-cellslice-local-arg.c (negative),
  multi-tu-gate-g5-cellslice-global-external.c,
  multi-tu-gate-g5-cellslice-global-negative.c (negative),
  multi-tu-gate-g6-cellslice-fn-external.c,
  multi-tu-gate-g6-cellslice-fn-negative.c (negative);
  test/EndToEnd/multi-tu-gate-g5-region-api.c,
  multi-tu-gate-g6-cellslice-fn-external.c)

- [x] FR-33 Cross-TU pointer-global with local use (W3.4 G8), and the G7
  retention. `importPointerGlobal` (CTS-P4) hard-rejected every REFERENCED
  externally visible pointer-typed global in a project import, because
  `globalPtrFacts` is merged per TU. G8 (this gate) fires only when the
  DEFINING TU itself references the pointer global — the pure-definition,
  used-elsewhere shared-header shape takes the referenced-only skip and is
  handled by `deferExternPointerGlobal` (FR-31). W3.4 G8 relaxes it with the
  SAME whole-program eligibility `deferExternPointerGlobal` uses: a single
  file-scope base bound project-wide with NO reassignment anywhere
  (`WholeProgramInfo::pointerGlobalBases` size 1 +
  `pointerGlobalSoleFileScopeBase` + no `pointerGlobalHasBodyRebind`) is
  reconstructible as the CTS-P4 single-base cursor global. A divergent
  cross-TU rebinding (the same pointer global bound to two different bases,
  or reassigned in a body) keeps rejecting — the single-base cursor model
  has no representation for a target that depends on which TU last ran.
  Legitimate under the closed-program assumption. Byte-identical crate-vs-
  native differential.
  (test/Import/C/multi-tu-gate-g8-ptr-global-external.c,
  multi-tu-gate-g8-ptr-global-negative.c (negative);
  test/EndToEnd/multi-tu-gate-g8-ptr-global-external.c)

  G7 (cross-TU function-pointer devirtualization) is RETAINED (gate kept,
  test pins the un-devirtualized shape). `planFnPtrAliases` devirtualizes a
  never-reassigned file-scope function pointer to a direct call only in a
  sole-TU import. The whole-program write set (`fnPtrGlobalsWritten`) already
  proves a `const`/never-written target is safe, but devirtualization
  REMOVES the `emitrust.global` — and a companion TU that only forward-
  declares the pointer (`extern int (*const p)(int,int);`) has no way to
  devirtualize its OWN call: it holds no initializer and no target
  `FunctionDecl`, so its reference to the now-absent global fails to resolve
  ("referenced but not defined in any translation unit"). Making it work
  needs the alias target threaded by symbol into every referencing TU plus
  direct-call emission without a `FunctionDecl` — disproportionate plumbing
  for a purely optimizing gate whose un-devirtualized fn_ptr + call_indirect
  is already correct and runs fine (a genuine optimization with thin demand;
  design.md's own "conservative-but-sound, disables OPTIMIZATIONS not
  correctness" framing). The G7 tests stay pinned at the sound
  un-devirtualized shape.
  (test/Import/C/multi-tu-gate-g7-fnptr-devirt-skip.c,
  multi-tu-gate-g7-fnptr-devirt-negative.c)

- [x] FR-34 Cross-TU function-pointer result erasure (W3.4 G1).
  `classifyFnPtrPointerResult` (CTS-S/CTS-P2, `ImportCTypes.cpp`) erases a
  data-pointer function-pointer RESULT type to `!emitrust.fn_ptr<()>` (routing
  the call through the returned function's single global base) when the set of
  address-taken candidate functions with a matching return type agrees on one
  base. That candidate set is built from THIS TU's per-TU
  `addressTakenFunctions` (refilled each TU by `planFnPtrAliases`), so in a
  multi-TU project a diverging candidate whose address is taken only in another
  TU could be missed — an unsound erasure — and the gate previously rejected
  EVERY fn-ptr result in a project import unconditionally. W3.4 G1 relaxes it
  with a whole-program candidate-completeness fact:
  `WholeProgramInfo::dataPtrReturnFnAddressTakenTus` maps each data-pointer
  return-type SPELLING (canonical `QualType::getAsString`) to the deduped set
  of TUs that take the address of a function returning that type, populated by
  the `collectWholeProgramInfo` pre-pass before any TU is classified. When at
  most ONE TU takes such an address, this TU's per-TU candidate set is provably
  the complete whole-program set and the existing per-TU classifier runs
  soundly (empty candidates → reject; one common base → erase; same-TU
  divergence → the precise disagreement wording). A narrow SPELLING key (never
  a `mapType` result) keeps the fact side-effect-free — no memoization
  poisoning (W3.2 COMMIT B's lesson) — and cross-TU-stable for named types.
  The relaxation is soleTU-guarded and multi-TU-pre-pass-only, so single-file
  imports are byte-identical. G1's `p` is used only in its defining TU, so the
  erased routing stays TU-local (`lookupGlobal(erasedBase)` resolves in-TU) and
  no cross-TU erased-base substrate is needed.

  DEFERRED (honest scope boundary): when TWO OR MORE TUs take the address of a
  matching-return-type function (a genuine cross-TU divergence, e.g. `get1`
  returning `&objA` in one TU and `get2` returning `&objB` in another through
  the same fn-ptr global), the gate keeps its BLANKET "function pointer result
  type" rejection rather than the precise "return sites disagree on the
  returned global base" wording. The precise cross-TU diagnostic needs the full
  erased-base substrate — each candidate's base symbol resolved whole-program —
  which this thin-demand gate does not build; the blanket reject is sound and
  honest. Same-TU divergence still uses the precise wording (the per-TU
  classifier runs whenever the whole-program count is ≤ 1). Legitimate under
  the closed-program assumption. Byte-identical crate-vs-native differential.
  (test/Import/C/multi-tu-gate-g1-fnptr-result-erasure.c,
  multi-tu-gate-g1-fnptr-result-diverge.c (negative);
  test/EndToEnd/multi-tu-gate-g1-fnptr-result-erasure.c)

- [x] FR-35 Cross-TU pointer struct member (W3.4 G2) — RETAINED.
  `resolveMemberPointerBinding` (CTS-P2, `ImportC.cpp`) rejects a pointer
  member of an externally visible struct instance in a multi-file project,
  because `memberPtrBindings` is merged per TU and another TU could rebind the
  member behind this TU's already-consumed facts. The CTS-P2 model stores the
  pointer member as a vestigial i64 and STATICALLY DEVIRTUALIZES every read to
  a direct reference to the bound target global (`head.next->val` lowers to
  `emitrust.global_load @tail`; the binding write `head.next = &tail` is
  elided). The natural cross-TU shape reads the member in a DIFFERENT TU than
  the one that binds it (the oracle's companion reads `head.next->val` while
  the base TU binds `head.next = &tail`). Accepting it is strictly harder than
  the G1/G8 relaxations, whose emission all stays TU-local: the consuming TU
  holds no `VarDecl` for the target `tail`, never parsed the binding, and would
  have to FABRICATE a by-symbol `global_load @tail` reference to a global it
  never declared, driven by a symbol-keyed member-binding fact reconstructed
  whole-program — the cross-TU-emission problem. This is disproportionate
  plumbing for a thin-demand shape whose sound rejection is already correct
  (the same "conservative-but-sound, disables a shape not correctness" framing
  as the G7 retention). RETAINED: the gate is kept, the located rejection
  pinned, and the corpus (Track 4) is the demand signal that would reopen it.
  The divergent two-base variant stays rejected regardless of disposition — the
  per-instance model has no representation for a target that depends on which
  TU last stored at link time.
  (test/Import/C/multi-tu-gate-g2-ptr-struct-member.c,
  multi-tu-gate-g2-ptr-struct-member-diverge.c (negative))
- [x] FR-36 Owner-index return, Stage 1 (FR-30 follow-on). `planOwners`
  previously disqualified a candidate method unconditionally on any pointer
  return type; it now proves, per candidate method AFTER the class's
  parameter-based union-find settles (so the check is a pure post-hoc
  refinement — zero behavior change for any program without this shape),
  that every `return` operand's root (`resolveArgRoot`, the SAME
  interprocedural resolution already used for call arguments) lands in the
  method's own class. A method that qualifies is recorded in the new
  `ownerIndexReturns` set (keyed by canonical declaration, disjoint from
  `pointerReturnKinds`/`globalReturnBases`, which a method never reaches)
  instead of being rejected; its Rust result type is a plain i64 element
  index. `emitReturnStmt` decomposes such a return exactly like a
  method-call pointer argument (`emitPointerRValue` plus the same
  rooted-at-owner/non-null/degenerate-base defensive checks
  `emitMethodCallSite` runs) and stores the cursor as the return value — a
  new `currentOwnerIndexReturn` flag keeps this disjoint from the
  pre-existing integer-carrier (CTS-P3) i64-return path, which classifies
  unrelated `void *` shapes to the same MLIR result type. Call-site
  consumption is symmetric: `PointerRegionAnalysis::recordPointerWrite`
  gains an `ownerIndexReturnQuery` callback (mirroring the existing
  `carrierReturnQuery` injection point) so `local = ownerIndexMethod(...)`
  re-classifies the call's first pointer argument (every pointer parameter
  of such a method shares one class, so any one determines the region) as
  the local's region source instead of the historical non-address
  rejection, and `emitPointerRValue` gained a matching `CallExpr` case that
  reuses `emitMethodCallSite` (extended with an optional out-parameter
  reporting the first pointer argument's resolved base) to materialize the
  call exactly once and pair its i64 result with that base as the local's
  (base, cursor) decomposition — so a local bound this way bins into its
  owning function's prologue and ordinary pointer-local machinery
  identically to one bound from an address form. Interprocedural
  propagation beyond the binding function (uniting a THIRD function's
  parameter class through a local itself bound from an owner-index-return
  call result, and thereby promoting that third function to a method too)
  is DEFERRED — no test in this stage exercises a call chain that deep, and
  `planOwners`'s own per-class promotion decisions are otherwise frozen
  before this refinement runs, so nothing regresses by leaving it for a
  later stage if the demand signal calls for it. Member pointers, enum
  synthesis, and the equality carve-out for owner-index-return results are
  explicitly out of scope for this stage.
  (test/EndToEnd/owner-index-return.c)
- [x] FR-37 Self-referential array-member struct field as enum, Stage 2
  (FR-30 follow-on), import-only. A new whole-program Pass-A pass,
  `planArrayMemberPointers` (run immediately after `planOwners`,
  consuming its `ownerPlans`/`methodPlans` output), proves for a
  self-referential data-pointer field (a field whose pointee is its own
  owning record type) of a promoted owner array's element type that
  EVERY arrow-form read or write of the field, anywhere in the program,
  roots — via the same `resolveArgRoot` interprocedural resolution
  `planOwners` and FR-36 both reuse — in that ONE owner array's class,
  and that every write's right-hand side is itself such a rooted value
  (the promoted cursor value itself, e.g. `x->self = x;`). A field with
  every site so proven is recorded in the new `arrayMemberPtrBindings`
  map (keyed by field declaration, since the proof is program-wide, not
  per struct instance like the pre-existing degenerate
  `memberPtrBindings`/`poisonedPtrFields` model it sits ahead of and
  falls through to unchanged when it cannot prove a field safe — this
  pass is strictly additive and never emits a diagnostic). A proven
  field's MLIR struct-field type becomes a synthesized
  `emitrust.enum_def` (one variant per array index, synthesized once per
  field on first use and memoized) instead of a plain i64 cursor; every
  WRITE lowers to a genuine `emitrust.switch` match over the i64 index
  being assigned (one case per element assigning that index's enum
  constant, plus a default — provably unreachable by the proof — that
  assigns the first variant, mirroring `IndexSwitchLowering`'s
  default-region convention); every READ decodes with the existing
  `castEnumToI32` helper widened to i64, with no branching, since the
  enum's storage IS the index by construction. `mapStructFieldType`
  gained an optional `FieldDecl*` parameter (its one caller, struct-def
  field emission, already had the field in scope) to consult the new
  map. Path compression / multi-step self-reference chains, the
  cross-parameter equality carve-out, and the differential end-to-end
  counterpart are explicitly deferred to later stages; this stage is
  import-only/FileCheck, validated byte-identically against every
  pre-existing member-pointer and owner-struct test
  (test/Import/C/pointers-member.c, pointers-member-base.c, owners.c,
  test/EndToEnd/owner-index-return.c, and the rest of check-emitrust).
  (test/Import/C/array-self-ref-member.c)
- [x] FR-37 Stage 3, differential end-to-end counterpart. Same
  `struct node { struct node *self; int x; }` shape as Stage 2, made
  runnable: `link_node` writes the enum-typed `self` field (the genuine
  `match` synthesized by `planArrayMemberPointers`) and increments the
  plain `x` field in the same method, proving the enum field and an
  ordinary field coexist; `main` loops over a 3-element promoted owner
  array, reading `self` back through `check_node`'s `p->self == p`
  equality (branchless enum decode) and printing both the equality
  result and `x` per element. Required zero `lib/` changes — the Stage 2
  mechanism was already runtime-correct; this stage only adds the test.
  Verified: native-vs-Rust stdout is byte-identical
  (`i=0 same=1 x=11` / `i=1 same=1 x=21` / `i=2 same=1 x=31`); the
  emitted crate contains both the `node_self_Bases` tuple-struct enum
  and a literal `match v0 { ... }` block; zero `unsafe` in the generated
  Rust. check-emitrust: 328/328 (up from 327/327).
  (test/EndToEnd/array-self-ref-member.c)
- [x] FR-37 Stage 4, path compression (chained self-reference). Extends
  Stage 2/3 from the degenerate `x->self = x;` shape to the actual
  `union-find.c` `uf_find` shape: `while (x->self != x) { parent =
  x->self; x->self = parent->self; x = parent; }`. Two sub-blockers,
  B3 and B4: B3 is `parent = x->self;`, a LOCAL bound FROM a field READ —
  `PointerRegionAnalysis::recordPointerWrite` (`lib/ImportC/ImportC.cpp`)
  gains a new case, gated by a new `arrayMemberFieldQuery` callback
  (mirroring FR-36's `ownerIndexReturnQuery` injection point), that joins
  the destination local into the arrow base's own class exactly like
  copying from the base directly — a parameter arrow base becomes the
  region base, a tracked local arrow base unions the two regions. B4 is
  `x->self = parent->self;`, a WRITE whose right-hand side is ITSELF an
  array-member field read rather than the cursor value itself (Stage 2's
  only proven write shape) — fixed entirely in Pass A
  (`planArrayMemberPointers`): a right-hand side that is an arrow read of
  a (candidate) array-member field is now as legal a write source as the
  cursor value itself, verified with the same read-side proof this pass
  already runs for every site of the field. The write-side EMISSION
  (`emitArrayMemberPointerAssign`) needed NO change: it already calls the
  general `emitPointerRValue` on its right-hand side, which already
  dispatches an array-member field read through
  `emitArrayMemberPointerRead` (Stage 2) regardless of context — B4 was
  purely a Pass-A proof gap, not an emission gap. The loop-comparison
  shape (`while (x->self != x)`) needed NO dedicated special case either
  (a structural deviation from the stage's plan, which anticipated one):
  `emitArrayMemberPointerRead` already returns
  `PtrExprValue{arrowBase->base, index}` (Stage 2), reusing the arrow
  base's OWN resolved base identity — here literally `x` on both sides of
  the comparison — so the general pointer-equality machinery's
  `lhs->base == rhs->base` identity check already passes trivially and
  the cursors compare correctly through the ordinary `arith.cmpi` path.
  `arrayMemberFieldQuery` is wired three ways: Pass A's own
  `PointerRegionAnalysis` (used while STILL proving the field, so it
  answers structural candidacy, not final `arrayMemberPtrBindings`
  membership) and both emission-time `PointerRegionAnalysis` instances
  (the ordinary and va_list-clone prologues), which answer proven
  membership. Explicitly out of scope and confirmed still rejected: the
  cross-parameter equality carve-out (`root1 == root2` on two DIFFERENT
  `find` results, `union-find.c`'s B5) and the full `union-find.c`
  program, both deferred to Stage 5/6 — `uf_union`'s
  `root1 == root2` / `root1->parent = root2;` shape still poisons Pass
  A's proof for the field program-wide (verified directly: adding
  `uf_union` to a minimal repro reproduces the exact pre-Stage-4 "used
  outside the static-binding model" rejection), independent of and
  unaffected by this stage's B3/B4/loop-comparison fixes. check-emitrust:
  330/330 (up from 328/328); zero `unsafe` in the generated Rust; every
  pre-existing member-pointer, owner-struct, and Stage 2/3 test
  unchanged.
  (test/Import/C/array-self-ref-member-chain.c,
  test/EndToEnd/array-self-ref-member-chain.c)
- [x] FR-38 Stage 5, cross-parameter pointer equality (B5). Fixes the two
  compounding gaps Stage 4 identified and confirmed still-blocking:
  (1) Pass A's OWN `PointerRegionAnalysis` instance
  (`planArrayMemberPointers`, `lib/ImportC/ImportCPlanning.cpp`) never had
  `ownerIndexReturnQuery` wired to it — only the emission-time instances
  were (FR-36) — so a local bound from an owner-index-returning call
  (`root1 = uf_find(node1);`) was invisible to Pass A's own root
  resolution and poisoned the field program-wide before the real,
  emission-time proof was ever reached; fixed by wiring the identical
  callback Pass A already wires for `arrayMemberFieldQuery` (FR-37).
  (2) Even once Pass A could resolve `root1`/`root2`, the general
  pointer-equality comparator (`lib/ImportC/ImportCExpressions.cpp`)
  rejected `root1 == root2` outright because each local's
  `PtrExprValue::base` is the DIFFERENT pointer parameter it was bound
  from (`node1` for `root1`, `node2` for `root2`) — genuinely different
  `clang::VarDecl`s even though, by `planOwners`'s all-or-nothing
  per-function qualification (every data-pointer parameter of one
  qualifying method shares one class), both are provably cursors into the
  SAME array class within that one function body. Fixed with a narrow
  carve-out immediately before the base-identity rejection: when both
  sides' bases are each either the current method's own owner array or a
  data-pointer parameter of the CURRENT method (`currentMethodOwner`,
  established once per function body — a base referenced from the body
  being emitted can only be a declaration visible to it, so no
  cross-function bookkeeping is needed), the base-identity mismatch is
  ignored for equality/inequality only and just the cursor (i64 index)
  values are compared — never generalized to ordered comparisons or
  across functions. A structural surprise beyond the stage's brief: once
  both fixes land, `test/RealWorld/Inputs/union-find.c` (the ultimate
  target) does not merely get past the `uf_union` blocker — the ENTIRE
  program now imports, converts, emits, builds, and differentially
  matches a clang-native build byte-for-byte (`test/RealWorld/realworld.c`
  ratchet updated forward to include `union-find`, the only manifest
  change this stage makes). check-emitrust: 332/332 (up from 330/330);
  zero `unsafe` in the generated Rust; every pre-existing member-pointer,
  owner-struct, and Stage 2/3/4 test unchanged.
  (test/Import/C/array-self-ref-member-cross-param.c,
  test/EndToEnd/array-self-ref-member-cross-param.c)
- [x] FR-38 Stage 6, `union-find.c` capstone differential test. Authors
  `test/EndToEnd/union-find.c` (it did not exist before this stage —
  only `test/RealWorld/Inputs/union-find.c` did) as a proper lit
  differential test: the real `struct uf_node { struct uf_node *parent;
  unsigned rank; }` program (`uf_node_init`, `uf_find` with path
  compression, `uf_union` with union-by-rank via B5 cross-parameter
  equality, `connected`) over an 8-element array, near-verbatim from the
  RealWorld input, printing the full 8x8 connectivity matrix and final
  ranks. Required zero `lib/` changes — Stages 2-5 already made the whole
  program runtime-correct; this stage only adds the regression-guarding
  test. Verified: native-vs-Rust stdout byte-identical; the emitted crate
  contains the `uf_node_parent_Bases` tuple-struct enum (one variant per
  array index) and multiple literal `match` blocks; zero `unsafe` in the
  generated Rust. check-emitrust: 333/333 (up from 332/332); the RealWorld
  ratchet (`test/RealWorld/expected-transpile.txt`, already updated by
  Stage 5) reverified independently via `run_realworld.py`: `union-find`
  TRANSPILED, zero regressions/improvements against the manifest, and the
  `self-ref-pointer-member` blocker tag no longer appears on any rejected
  corpus program. `test/RealWorld/Inputs/binary-tree.c` (separately
  tagged `returned-pointer`) was re-checked directly and still rejects,
  unchanged, at its `malloc`'d-node return site — confirmed a C99-46
  (dynamic memory) blocker, not a member-pointer or owner-index-return
  gap; left untouched, out of scope.
  (test/EndToEnd/union-find.c)

- [x] FR-39 Dynamic memory / index-handle node pool (RFC, C99-46 Stage 1).
  A two-part wave (W4.2e) that gives the importer its first heap-allocation
  support under the no-`unsafe`, no-heap-in-the-model discipline. The RFC
  record (user-approved, "full `Option<usize>`"): the C heap is never
  reconstructed with `Box`/`Rc`; instead a disciplined allocation pattern
  is proven statically and lowered to a fixed-size backing the ordinary
  region machinery already models, so the emitted Rust stays pure safe
  array indexing. The soundness of `free` as a no-op rests on a defined C
  program never reading freed storage; the differential (native vs crate,
  byte-identical) is the oracle for every capstone.
  - **Part A -- local flat buffer (non-RFC).** `T *p = malloc(N*sizeof(T))`
    bound to a LOCAL pointer, where `N` folds through a new foldable-local
    resolver (`evalFoldableInt`: an automatic local never reassigned nor
    address-taken folds to its initializer, so the `cap*sizeof(int)` idiom
    resolves). Synthesizes a mutable entry-block `[T; CAP]` backing + an
    i64 cursor -- the writable, function-scope analog of a string-literal
    region; `p[i]` subscripts the backing directly. `malloc`'s
    indeterminate contents are refined to zero (matching `calloc`).
    Rejections (each pinned, located): non-constant size, a returned
    pointer (the dangle reject), an escaping pointer, `realloc`, and `free`
    of an unrecognized pointer. Capstone: `test/EndToEnd/malloc-stack.c`.
  - **Part B -- index-handle node pool (RFC).** A `malloc(sizeof(struct T))`
    inside a foldable-trip-count loop building a self-referential linked
    structure, where `struct T` has exactly one self-ref pointer field,
    every `struct T *` local roots in this one pool (bound to a malloc
    result, another such local, a self-ref field read, or NULL), nothing
    escapes, and `free` is applied only to such locals. A new Pass-A
    `planMallocPool` proves this per function (conservatively -- an
    unprovable function stays unpromoted and falls through to the existing
    located rejection, never a miscompile) and records a VarDecl-less
    `MallocPoolFacts`. The function synthesizes one shared `[T; CAP]` pool
    (CAP = the loop trip count) + a free cursor; each node pointer becomes a
    nullable pool-index HANDLE -- an i64 index cell + an i1 non-null cell
    (the CTS-P8 shape) decomposed against the shared pool as its backing,
    so `h->field` reuses the ordinary subscript+member projection with a
    null-deref `assert!` guard. The self-ref field renders as
    `Option<usize>`; the (non-null, index) handle pair is built into and
    destructured out of that field through the one-per-module
    `__emitrust_pool_opt`/`__emitrust_pool_unpack` helpers. `malloc`
    appends a zeroed slot at the cursor; `free` is a no-op. NOT the
    per-index enum of FR-37/38 (a pool null-checks but never compares
    nodes, so no exhaustive match is needed) and NOT `Vec<T>` (the fixed
    `[T; CAP]` needs a foldable capacity; `Vec` is the future
    generalization when CAP is unfoldable). Rejections (pinned): an
    unbounded/non-foldable capacity, a returned/escaping node (keeps
    `binary-tree` out -- it is doubly blocked by returned-pointer), a
    node-pointer used outside the null-check-only handle model, and a
    second distinct pooled struct type. Capstone:
    `test/EndToEnd/linked-list.c`.
  - **Future direction (preferred target representations).** The roadmap
    for the remaining dynamic-memory shapes maps allocation patterns onto
    idiomatic, safe Rust standard collections rather than any custom
    ownership machinery: a fixed foldable-capacity buffer stays a Rust
    `array` (`[T; CAP]`, shipped here); an unfoldable/growing capacity
    generalizes to `Vec<T>` (an index-handle arena over `Vec` -- same
    nullable-`Option<usize>` handle, capacity checked at push instead of
    folded); and a producer/consumer or FIFO allocation shape maps onto a
    Rust queue -- `VecDeque<T>`, or `std::sync::mpsc` when the pattern
    crosses a thread boundary. Each keeps the emitted Rust pure safe (no
    `unsafe`, no raw pointers); the index-handle model generalizes across
    all three because the handle is always a collection index, never an
    address.
  - **Architecture decision: recognition stays at the AST level, NOT an MLIR
    pattern rewrite (spike, NO-GO).** A spike assessed moving the
    malloc→container RECOGNITION into an MLIR pattern-rewrite pass (let
    allocations survive import as `emitrust.alloc`/`free` ops, match the idiom
    on the IR). NO-GO, on two measured blockers: (1) a C counted
    `for (i=0;i<N;i++)` does not lift to `scf.for` -- at the pass insertion
    point (`mem2reg → canonicalize → lift-cf-to-scf → canonicalize`) it is an
    `scf.while` with the induction variable as a loop-carried value (bound and
    increment buried in the before-region), and no standard MLIR pass raises
    `scf.while`→`scf.for`, so CAP recovery needs fragile, shape-specific IV
    analysis the AST-level `foldLoopTripCount` gets for free; (2) the emitrust
    dialect has no un-decomposed data-pointer type (a data pointer is
    decomposed to (base, cursor) / a node handle is two cells + a shared
    backing before any op is emitted), so an `emitrust.alloc` with a
    pointer-typed result cannot be produced without rebuilding the pointer model
    at the IR level. Recognition is a PRECONDITION for emission, so there is no
    IR to match. The pattern MACHINERY is fine (idiom fusion already exists,
    e.g. FuncToEmitRust's addr_of+call→method_call) -- only IR-level recognition
    fails. The declarative-extensibility goal survives via a **fat-op split**:
    the importer recognizes at the AST and emits a high-level container-intent
    op carrying the recovered facts (cap, shape, elem type) as attributes; a
    pattern-rewrite pass LOWERS that op to the concrete container, one pattern
    per container -- both blockers sidestepped (constant cap, resolved shape).
    That is the only container work that should be a pattern rewrite.
  - **Fat-op split proven GENERAL (spike, GO).** A throwaway spike (uncommitted;
    no lib/ commit) proved the fat-op split is a reusable *container
    computational model*, not a node-pool one-off: **one** high-level
    representation lowered by **two** independent patterns to correct Rust.
    Three throwaway ops captured the model of "nullable indices into a
    collection of `T`" -- `emitrust.collection` (a collection place carrying an
    `element_type` attr and, for array, a folded `capacity` attr),
    `emitrust.collection_push -> i64` (append a defaulted `T`, return its
    index), and `emitrust.collection_at(%c, %idx) -> lvalue<T>` (element place);
    the collection place used a tagged `emitrust.opaque<"__spike_collection">`
    value type until lowered (no new dialect *type* needed -- the FR-39 verifier
    relaxation already admits `OpaqueType`). A greedy `RewritePattern` pass
    (`spike-container-lowering`, `applyPatternsGreedily`, `backend=array|vec`
    option, inserted before `convert-to-emitrust`) lowered ONE hand-authored
    linked-list kernel (create / push in a loop / link via the `Option<usize>`
    next field / traverse-sum / print) two ways: `array` -> `emitrust.variable
    <[Node;5]>` + i64 cursor + `emitrust.subscript` (reproducing FR-39's shape,
    cursor-bump push over pre-defaulted slots); `vec` -> `emitrust.variable
    <Vec<Node>>` + `.push(Node::default())` + `len()-1` + bracket-index.
    Differential per backend (same oracle as FR-39 / the RealWorld corpus):
    both crates build `cargo build --release --offline`, run, and byte-match a
    `clang -std=c11` native build (both print `10`); the array backend has zero
    `unsafe`, the vec backend is safe `Vec` (a real heap allocation, throwaway).
    The generality is decisive on three counts: (a) the `capacity` attr is used
    by the array pattern and IGNORED by the vec pattern (`push` grows) -- no
    backend-specific detail leaks into the high-level ops; (b) the `Option
    <usize>` next field and the `__emitrust_pool_opt`/`_unpack` handle helpers
    are shared verbatim across both backends (an index is an index whether the
    collection is an array or a `Vec`); (c) adding `VecDeque`/`mpsc` later is
    one more pattern over the same representation, recognition untouched at the
    AST. **Production split LANDED (W4.2e follow-on).** The importer now emits
    the three container ops at malloc-pool recognition time (`planMallocPool`
    reused verbatim, only emission moved), and a committed **array-only**
    lowering pass (`emitrust-lower-containers`, run first in `emitrust-cc`'s
    pipeline before `mem2reg`) turns them back into the exact legacy `[T;CAP]`
    pool + memref i64 cursor + `emitrust.subscript` shape -- zero behavioral
    change (linked-list still byte-matches native, zero `unsafe`; 347/347 lit;
    RealWorld ledger unchanged). The array backend is THE production container
    path; the spike's `vec` pattern + the `backend` option were dropped (a
    growable `Vec`/heap backend stays RFC-gated at W4.5, a memory-model change
    needing explicit sign-off). The placeholder collection value type is
    `emitrust.opaque<"__emitrust_collection">`. W4.2e Part A (the flat-buffer
    local `malloc`) does not fit the push/at model and stays inlined as-is.
  - The `Option<usize>` field required relaxing `emitrust.struct_def`'s
    field-type verifier to admit `OpaqueType` (the importer guarantees any
    such field is `Copy + Default`; a non-`Copy` opaque like `Vec<T>` is
    never emitted as a field). RealWorld corpus after this wave: 9
    transpiled (adds `malloc-stack`, `linked-list`), `binary-tree` still
    rejected (returned-pointer). Gates: c-testsuite ledger 220/220
    no-drift; 150-seed checker-on fuzz clean; CTS-P4 global-malloc and the
    whole C EndToEnd corpus byte-identical (Part B is strictly additive --
    only the new pool shape is promoted).
  (test/Import/C/malloc-local-flat.c, test/Import/C/malloc-pool-list.c,
  test/EndToEnd/malloc-stack.c, test/EndToEnd/linked-list.c)

- [x] FR-40 Project item graph (W5.0). A pure-AST, whole-project dependency
  graph over PROGRAM ITEMS, built before any IR exists, in a new
  `lib/Project/` library independent of `CImporter`'s IR-building state.
  A node is one top-level item: a function definition or prototype, a
  record (`struct`/`union`/`class`), an enum, or a global variable, keyed
  by the same symbol name the importer will emit (`mlirFuncName` /
  `globalVarSymbolName` semantics, including the W2.0 namespace-flattening
  prefix and the per-TU `static` mangle) so a graph node and an emitted
  Rust item are the same thing by construction. Edges, all directed
  item→dependency: `Calls` (a call expression in a body), `SigType` (a
  record/enum named in the return or a parameter type), `BodyType` (a
  record/enum named by a local, cast, or `sizeof` in the body),
  `Field` (a record's field type), `Base` (a C++ record's base class),
  `ReadsGlobal`/`WritesGlobal`, and `TakesAddressOf` (a function whose
  address is taken). The graph is the SEARCH SPACE of FR-43: "expand via
  the call graph" is a traversal of `Calls`, and "type coloring" (FR-41)
  is a fixpoint over `Field`/`Base`/`SigType`. Deterministic iteration
  order (symbol name, then TU index) is a hard requirement — the search
  above it must be reproducible. Exposed for testing through a new
  `emitrust-cc --emit=item-graph` mode printing one stable line per node
  and per edge.
  (test/Project/item-graph-*.c, test/Project/item-graph-cpp.cpp)

- [x] FR-41 Item coloring (W5.1). A three-color lattice over FR-40's items,
  computed by fixpoint, that decides what a partial port can contain:
  **Green** (the item and its whole type closure are inside the supported
  subset), **Yellow** (the item itself is admissible but at least one
  CALLEE is Red — it is still emitted, calling a stub), and **Red** (the
  item is itself inadmissible, or any type it structurally depends on is
  Red). The asymmetry between the two poison rules is the load-bearing
  design point and follows directly from what Rust lets you write: a
  missing FUNCTION can be replaced by a body-less stub with the right
  signature, so call-poisoning only demotes a caller to Yellow; a missing
  TYPE cannot be replaced at all, because its size, its fields, and its
  `Default`/`Copy` derives are load-bearing at every use, so type-poisoning
  propagates transitively and turns every dependent Red. Seeds come from a
  conservative, pure-AST ADMISSIBILITY PROBE per item — a syntactic screen
  for the constructs the importer already rejects by design (templates,
  virtual methods, base classes, reference types, exceptions,
  pointer-to-pointer, unsupported top-level decl kinds) — deliberately
  UNDER-approximating: the probe may call an item Green that the real
  import later rejects, and FR-43's search is what repairs that, so the
  probe must stay cheap and must never call an importable item Red.
  Exposed through `emitrust-cc --emit=coloring`.
  **AMENDMENT, from the implementation.** This entry described the two
  poison rules separately; they are ONE rule, and stating it that way is
  what makes the model correct: an edge to a Red target poisons its source
  RED unless the target is STUB-REPLACEABLE, in which case only YELLOW —
  where stub-replaceable is quoted from FR-42's actual recovery policy (a
  rejected function whose signature still maps becomes an
  `unimplemented!()` stub; everything else is dropped). Consequences, all
  pinned by tests: `BodyType` poisons RED (the body mentions the type, so
  those statements cannot be written — Yellow would claim the function
  emits, which is false), but the SIGNATURE never named it, so the
  function stays stub-replaceable and ITS callers are only Yellow — Red
  bodies stop exactly at the function boundary, which is where a stub can
  be inserted. `SigType` poisons RED *and* unstubbable, because the stub
  would have to spell the missing type in its own parameter list, so such
  a function's callers are Red, NOT Yellow — correcting this entry's
  original claim that call-poisoning always yields Yellow. And
  unstubbability does NOT propagate, only Red does: a caller of a
  signature-broken function is Red but is itself stubbable, so ITS caller
  is Yellow. That last rule is what stops one unsupported type from
  reddening a whole program.
  Blame is actionable, not just a color: each non-Green line carries the
  immediate poisoner, the edge that carried it, the full chain to the
  inadmissible root, and that root's construct tag.
  The probe's deliberately-kept-Green list is FR-43's work list. It does
  NOT screen pointer-to-pointer parameters: `const char **` string cursors
  (CTS 00204) and `main`'s `char **argv` both import today, so the syntax
  decides nothing and screening them would have been a false Red.
  Validation: all 220 c-testsuite expected-pass cases plus the whole
  `test/Import` and `test/EndToEnd` corpus colour ALL-GREEN — zero false
  Reds or Yellows; `fixed-stats` 11/11 and `tokenizer` 5/5 Green, matching
  the fact that both fully transpile. Determinism checked beyond the lit
  fixture with a temporary seeded shuffle of nodes AND edges: 88 runs, zero
  mismatches.
  (test/Project/coloring-green.c, coloring-yellow.c, coloring-red-type.c,
  coloring-cycle.c, coloring-determinism.c, coloring-compdb.c,
  test/Project/coloring-cpp.cpp)

- [x] FR-42 Recoverable import (W5.2). Today `importDeclsIn` returns
  `failure()` at the first unsupported declaration, so one unsupported
  construct anywhere in a project yields NO output at all — the single
  reason a real C++ project produces nothing today, and the thing
  "incremental progress" has to fix. `CImporter` gains an off-by-default
  RECOVERY MODE: a rejected top-level item is recorded in a
  `RejectionLedger` (emitted symbol, `FileLineColLoc`, the verbatim
  diagnostic, and the RealWorld blocker tag) and the walk CONTINUES to the
  next item instead of aborting. A rejected function whose signature is
  fully mappable is emitted as a STUB — the real signature, a body of
  `unimplemented!("<reason>")` — so callers still compile; one whose
  signature is not mappable is dropped entirely and its callers are
  themselves demoted, which is exactly FR-41's Red/Yellow distinction
  observed dynamically. Any IR partially built for a rejected item is
  discarded before the walk resumes; a half-built item must never reach
  the module. In recovery mode the located diagnostics are emitted as
  WARNINGS, not errors, so the driver still exits 0 with a partial crate.
  The mode is off by default and every existing single-shot path keeps
  byte-identical output — the gate is a byte-identical `--emit=rust`
  snapshot over the whole existing EndToEnd corpus.
  (test/Import/C/recover-*.c, test/EndToEnd/recover-partial.cpp)

- [x] FR-43 Frontier tree search (W5.3). The driver that turns FR-40/41/42
  into a maximal partial port. A search STATE is a set of admitted items
  plus a representation choice per admitted item; expansion is best-first:
  attempt a recovering import (FR-42) of the current state, and every
  rejection it reports is a LEARNED FACT that the coloring probe missed,
  so each child state drops the offending item and re-colors its
  dependents. The score is lexicographic — items emitted for real, then
  negated stub count, then summed representation cost. Bounded and
  reproducible: `--max-search-nodes` (default 8, counted in IMPORTS),
  memoization on the admitted-set hash, symbol-name tie-breaks. The
  representation dimension ships with exactly ONE candidate per item —
  today's greedy Pass-A planners — so this wave changes no existing
  output; the FR-39 container fat-op split (array pool vs `Vec` vs
  `VecDeque`, W4.5) is where alternatives plug in.
  Surfaces: `--search` (with `--emit=crate --incremental`), `--emit=search`
  (trace only — no crate and no `main` required, so a LIBRARY is
  analyzable), `--search-trace`. An excluded item is implemented as a
  SYNTHETIC REJECTION in `importTopLevelDeclRecovering`, so it stubs when
  its signature maps and drops otherwise — no new outcome kind anywhere in
  the importer.
  **AMENDMENT, measured.** This entry originally specified the root state
  as the Green-union-Yellow closure OF THE ROOTS. That is wrong under
  FR-41's rule: the coloring is a global least fixpoint, and intersecting
  it with root-reachability discards portable items whose only referrer is
  Red (`polygon`'s `tu0_abs_int`, `shapes`' `tu2_isqrt`) — and in both of
  those projects EVERY root is itself Red, so the root-closure candidate
  set is empty, taking the corpus from 19/31 to 16/31. The root state
  therefore admits all Green-union-Yellow items, and the roots are used
  only to ORDER repairs (give up what the entry points lean on least).
  **HONEST NEGATIVE RESULT on the C++ corpus.** 19/31 with the search off
  and 19/31 with it on — not one item better. All four projects explore
  exactly one state. The reason is precise, and is a credit to FR-41
  rather than a defect in FR-43: the coloring is EXACT on this corpus
  (green counts 11/11, 5/5, 2/8, 1/7 equal the achieved ported counts), so
  there are no false Greens to repair, and `polygon`/`shapes` are blocked
  by per-item rejections — `std::vector` references, inheritance and
  destructors — that no choice of admitted set can move. The search
  repairs false Greens and whole-program failures; this corpus has
  neither. Where it DOES pay is measured in
  `test/Project/search-backtrack.cpp`: a recovering import that dies
  whole-program in `finalizeProject` yields NO crate under `--incremental`
  alone, and 4 of 5 items ported plus a compiling crate under
  `--incremental --search`. Attribution there enumerates a whole failure
  CLASS from the graph rather than chasing the single symbol
  `finalizeProject` happened to name first.
  Cost: 1.08x-1.42x on the corpus (one extra import; FR-44's denominator
  now reuses the search's graph instead of re-parsing). Worst case
  measured at 4 probes (`search-false-green.c`), so a project heavy in
  body-level rejections can burn the full budget for no gain —
  `--max-search-nodes=1` or `2` is the recommendation for large projects.
  The default stays 8 because a rejected item can leave importer state
  that breaks a later one, and only a probe settles that.
  (test/Project/search-green.c, search-red.c, search-bound.c,
  search-cli.c, search-determinism.c, search-false-green.c,
  test/Project/search-backtrack.cpp)

- [x] FR-44 Incremental crate output and progress ratchet (W5.4).
  `emitrust-cc --emit=crate --incremental` writes a crate that BUILDS from
  a project only partially inside the subset, plus the report that makes
  the progress legible: `PORTING.md` (one row per item — symbol, color,
  blocker tag, source location) and `emitrust-progress.json` (the same,
  machine-readable, with totals). Progress is ratcheted the same way the
  c-testsuite ledger and the RealWorld manifest already are: a per-project
  `expected-items.txt` records which items are expected to port, and a
  two-way check fails on regression and prints improvements to be ratcheted
  forward with `--update`. This is what makes the work INCREMENTAL in the
  operational sense — each later wave is measured by how many items move
  from Red/Yellow to Green on a fixed corpus, and no wave may silently
  lose ground.
  Landed: `PORTING.md` plus `emitrust-progress.json` (schema
  `emitrust-progress/1`, documented in `ProgressReport.h`), rendered by
  pure functions with the driver doing the I/O, preserving the
  `CrateEmitter` split. The denominator is the FR-40 item graph, so
  "k of n items ported" is real rather than ledger-only, and status comes
  from a three-way join — graph nodes (what exists) x rejection ledger
  (what failed) x the emitted module's symbol table (evidence a survivor
  actually became Rust) — so `ported` is VERIFIED, not inferred.
  Graph-vs-emitted-symbol mismatches are handled explicitly: multi-TU
  header rejections collapse on (symbol, file, line, column); `extern`
  prototypes count as `declared` and leave both numerator and denominator;
  and C++ member functions, which are not graph nodes, are deliberately
  kept OUT of the denominator with their own off-graph tally — they are
  invisible when they succeed, so counting them only when they fail would
  make a project look WORSE the more of it ported.
  This work also exposed and fixed an FR-42 defect: `classifyBlocker` was
  missing the C++ substring and node-name tables its own header claims are
  identical to `run_realworld.py`'s, so every C++ rejection tagged `other`
  — which made the ranked blocker table useless for exactly the projects
  it exists to serve.
  **Baseline, untuned and checked in: `fixed-stats` 11/11, `tokenizer`
  5/5, `polygon` 2/8, `shapes` 1/7 — corpus 19/31 (61.2%).** `polygon` and
  `shapes`, both hard rejections before this, now emit crates that
  `cargo build --release --offline` compiles.
  (test/Driver/incremental.c, test/EndToEnd/incremental-builds.cpp,
  test/RealWorld/Cpp/Inputs/*/expected-items.txt)

- [x] FR-45 `compile_commands.json` input (W5.5). Real C++ projects are
  described by a compilation database, not by an argv list of sources plus
  hand-copied `-I` flags, which is all `emitrust-cc` accepts today.
  `--compdb <dir-or-file>` loads a `JSONCompilationDatabase` and derives
  both the source list and each file's own command line from it,
  superseding `PerFileCompilationDatabase`'s extension-based language guess
  when a real entry exists (the guess stays as the fallback for files the
  database does not mention). Driver-only arguments that LibTooling must
  not see (`-c`, `-o`, `-M*`) are filtered. Cherry-picked from
  `verified_transpilation_pipeline`'s `parse_compilation_database.rs`,
  which is the only piece of that prototype directly portable here — the
  rest of it (a whole second Rust-side C parser, a Z3 verification-condition
  layer) is a parallel architecture, not a component this one can absorb.
  (test/Project/compdb-*.c)

- [x] FR-46 C++ project corpus (W5.6). `test/RealWorld/Cpp/` extends the
  Track 4 demand-signal corpus with small, realistic, deterministic C++
  PROJECTS (several TUs plus headers and a `compile_commands.json`) whose
  job is to generate demand for the C++ input subset the way the C corpus
  did for the pointer model. Scored by FR-44's ratchet — percent of items
  Green — rather than pass/fail, precisely because none of them will port
  completely for a long time; a project whose ported fraction goes UP is
  the wave's evidence, and the blocker-tag tabulation over the Red items
  is the ranked backlog that sequences the C++ waves after this one.
  Landed: four projects — `fixed-stats` (namespaced free functions, plain
  structs through pointers, `extern "C"`; deliberately near-in-subset),
  `tokenizer` (two classes, member-initializer-list constructors, mutating
  and `const` methods), `polygon` (`const std::vector<T>&` and `T&`
  parameters, range-for), `shapes` (abstract base, virtual dispatch through
  a base pointer across TUs). `run_realworld.py` was EXTENDED rather than
  forked — the outcome model, quarantine, timeout, differential oracle and
  tag tabulation are shared, with `--corpus-kind {c,cpp}` and
  `--manifest-format {names,outcomes}` as the only dispatch points; the C
  path is behaviorally unchanged (9 transpiled / 4 rejected, same four
  tags). A latent tagging bug was fixed in passing: `first_line(stderr)`
  returned ClangTool's `[k/n] Processing file` progress chatter on any
  MULTI-TU rejection, tagging every such program `other` — latent for the C
  corpus only because both its multi-TU programs transpile.
  **Baseline: 1 transpiled, 3 rejected.** `fixed-stats` already transpiles
  and byte-matches a `clang++ -std=c++17` build.
  (test/RealWorld/Cpp/, test/RealWorld/run_realworld.py)

**W5.6 ranked C++ backlog** (from the FR-46 baseline; each project stops at
its FIRST blocker, so a tag walks forward as each one is cleared):

| Rank | Blocker | Project | First diagnostic | Cost |
|--|--|--|--|--|
| 1 | implicit-`this` method receiver | `tokenizer` | `unsupported assignable expression: CXXThisExpr` | low |
| 2 | `cxx-references` | `polygon` | `unsupported: reference types are not yet supported` | med |
| 3 | `cxx-destructor` → bases → virtual | `shapes` | `unsupported: user-declared destructor` | high |

Rank 1 is the survey's key finding and was NOT a documented gap: W2.2
landed `CXXThisExpr` as an rvalue and as a member-access base, but an
intra-class method call (`finish();` — an implicit-`this` receiver) needs
`addr_of` of the receiver PLACE, and `CXXThisExpr` has no assignable-place
case. Minimal repro: `class A { void bump() { v_ = v_ + 1; }
void both() { bump(); bump(); } int v_; };`. This blocks any class whose
methods call each other — pervasive in real C++ — at a fraction of the cost
of references or inheritance, so it precedes both.

- [x] FR-47 Implicit-`this` method-call receiver (W5.7, rank 1 above).
  Gives `CXXThisExpr` an assignable-place case (`emitCxxThisPlace`, in
  `emitLValue` immediately after the `UO_Deref` branch) so
  `emitCXXMemberCall` can take `addr_of` of the implicit receiver.
  `(*this).bump()` already worked — it is a `UnaryOperator` reaching
  `emitDerefLValue` — so the fix had a pinned target shape to reproduce
  rather than a new one to invent.
  A SECOND root cause the W5.6 survey did not name: once the place
  existed, a method calling a sibling DECLARED LATER still failed with
  "call to unimported method", including the very common
  `A() { init(); }`. `importCXXMethods` was single-pass declaration-order,
  but a C++ member function body is a complete-class context, so it
  becomes declare-then-define (`importFunction` gains `signatureOnly`,
  implemented by clearing `isDefinition` so a stub and a definition derive
  their signature from one code path). This is orthogonal to `this` — it
  reproduces identically with an explicit receiver — but "any intra-class
  method call" is not true without it. Alternatives rejected and recorded
  in the header: on-demand callee import recurses forever on mutual
  recursion; a topological sort has no order for mutual recursion; hoisting
  one shared `this` deref into the prologue would not dominate
  nested-region uses.
  Receiver mutability REUSES the existing rule rather than adding a second:
  `emitCXXMemberCall`'s `is_mut = !method->isConst()`, paired with the
  receiver type `importFunction` already fixes at signature time.
  `emitCxxThisPlace` returns a borrow-agnostic place and makes no
  mutability decision of its own.
  Still rejected, each pinned and each rejecting identically with an
  EXPLICIT receiver (so none is an implicit-`this` limitation): a sibling
  call into a virtual method, a class-declared method never defined in any
  TU (the new prepass creates that stub), and passing the receiver on by
  reference (`helper(*this)`).
  Gates: c-testsuite ledger inert at 220/220; byte-identical `--emit=rust`
  over all 91 `test/EndToEnd/*.c`. **`tokenizer` REJECTED -> TRANSPILED**,
  differentially validated against its `clang++` build; the C++ corpus is
  now 2 transpiled / 2 rejected and the
  `unsupported-assign-expr:CXXThisExpr` tag is retired. Next ranked
  blockers: `cxx-references` (`polygon`), `cxx-destructor` (`shapes`).
  (test/Import/Cpp/cpp-implicit-this.cpp, test/EndToEnd/cpp-method-chain.cpp)

- [x] FR-48 C++ reference types (W5.9, rank 1 of the W5.6 backlog).
  `const T&` and `T&` PARAMETERS map onto the EXISTING FR-28
  pointer-parameter classification rather than a parallel path: a C++
  reference is a pointer that is non-null, never reseated and never
  arithmetic — a strictly simpler case of machinery the importer already
  has. Reference RETURNS and reference MEMBERS stay rejected with located
  diagnostics; lifetime and ownership for those are not settled by this
  wave. Binding a prvalue to a `const T&` (`twice(a + b)`, `twice(7)`)
  stages the value into a fresh local and borrows that, reproducing the
  C++ full-expression lifetime as `let t = ...; f(&t)` — sound because the
  borrow is SHARED, so a temporary has no other name, nothing can observe
  it, and there is no caller-visible write to lose; C++ forbids binding a
  prvalue to `T&`, so the mutable case is unreachable (guarded anyway).
  An independent review hand-traced aliasing (`f(x,x)`, `a.m(a)`,
  `a.m(a.field)`, reborrow across nested calls, reference-to-reference
  forwarding), the `emitBorrowArgument` discriminator, C-path neutrality
  of the `placeExprRoot` refactor, and `const T&` write-through, and found
  no soundness issue. KNOWN GAP, fails closed rather than miscompiling: a
  reference-to-function-pointer parameter is accepted by `mapParamType`
  but its argument path ends in a located "unsupported pointer
  expression".
  **Measured: `polygon` 2/8 -> 4/8**, its first blocker walking forward
  from `cxx-references` to `unsupported-stmt:CXXForRangeStmt` (3 items);
  corpus 19/31 -> 21/31. Byte-identical `--emit=rust` over all 91
  `test/EndToEnd/*.c`; c-testsuite inert at 220/220.
  (test/Import/Cpp/cpp-references*.cpp, test/EndToEnd/cpp-references.cpp)

- [x] FR-49 Root-cause blocker attribution (W5.8). Measured defect:
  `--incremental` on `shapes` reported `other 13`, every one of them the
  identical `unsupported: method of an unimported class` — untagged, and a
  CASCADE SYMPTOM rather than a cause. Those 13 methods are unimportable
  only because `Shape` has a user-declared destructor and three siblings
  have base classes, so the artifact a person reads to choose work pointed
  at 13 derived symptoms instead of 4 real roots. FR-41 already computes
  the poison chain and the root construct tag; this JOINS it into the
  report rather than recomputing it. Each item now carries its direct
  blocker AND its root blocker plus the chain, so attribution is
  auditable, and the table ranks by root cause while keeping the direct
  tally. `shapes` now reads `base-class 16 / destructor 3` against a direct
  `cxx-cascaded-method 13`.
  Off-graph member functions (not item-graph nodes) attribute to their
  enclosing CLASS's node via a new `RejectedItem::ownerSymbol` recorded by
  the importer at rejection time — NOT recovered from the symbol, because
  it cannot be: nothing in `area_x100` says `Rect`, and three sibling
  classes each define one. Schema deliberately NOT bumped: the additions
  are strictly additive and `blockers` keeps its FR-44 meaning, so
  `run_realworld.py`'s ratchet reads the same keys with the same values.
  Emitted Rust byte-identical over all 102 EndToEnd inputs; corpus
  fractions unmoved — attribution changes the REPORT, not what ports.
  (test/Driver/incremental-root-blockers.cpp)

- [x] FR-50 `--search` never loses to `--incremental`, plus a real
  miscompile fix (W5.10). Opened from a measured regression — but **the
  premise was partly wrong, and the truth was worse**. The trigger was
  `--incremental` reporting 4/5 ported where `--incremental --search`
  reported 3/5, read as FR-43's search losing a portable item to an FR-41
  false Red. The 4/5 was a FICTION: `Holder` emitted as
  `{ d: Derived, k: i32 }` referencing a struct that had been dropped, and
  `rustc` rejects that crate with `E0412`. That fourth item never existed.
  Root cause, more serious than the reported symptom: `importedRecords`
  was marked BEFORE the field walk, so a rejected record still handed back
  an emitted name and `mapType` built `!emitrust.struct<"S">` for a struct
  nobody defines. `importRecord` now remembers rejections
  (`rejectedRecords`) and fails again at the use site so the rejection
  CASCADES — exactly as FR-41 predicts. This materially strengthens
  FR-42/FR-44's "the incremental crate compiles" claim, which was
  previously violable by ANY dropped record with a by-value dependent, and
  without it the new safety net would have preferred the broken crate.
  (a) **Safety net.** The plain recovering import is itself a state in the
  search space — the one whose `excludedItems` is empty — so
  `frontierSearch` probes it unconditionally as node 1 and scores it with
  the existing rule. Since `best` is a maximum over probed states and the
  baseline is always one of them, the property is STRUCTURAL, not
  aspirational. Asserted in `frontierSearch` AND checked unconditionally in
  `emitrust-cc` with a loud internal error plus fallback, because `assert`
  compiles out of Release — exactly where users rely on the guarantee. The
  budget floor rises to 2 so `--max-search-nodes` cannot switch the
  guarantee off; cost stays one probe when baseline == root.
  (b) **The coloring rule.** A `Field` edge poisons iff the emitted Rust
  SPELLS the target: by-value, array-of, and function-prototype fields do;
  a POINTER field does not (it emits as `i64`), and becomes a new
  `FieldIndirect` edge kind — appended to the enum so every existing
  enumerator keeps its value and all golden `--emit=item-graph` output is
  untouched. `SigType` was re-checked empirically and IS exact (a
  `struct Atom *` PARAMETER emits as `&mut Atom` and does name the record,
  so there is no pointer exemption there). `Base` is vacuous — a record
  with a direct base is already an inadmissible seed.
  KNOWN REMAINING false Red, deliberately left: `BodyType` through a
  pointer. It costs one STUB-REPLACEABLE function (callers stay Yellow)
  and the safety net now recovers it — verified, both cases show the
  baseline winning.
  The asymmetry is now recorded in `ItemColoring.h` where it belongs:
  **a false Green costs a probe, a false Red costs items permanently.**
  Gates: `--emit=rust` and `--emit=crate --incremental` both
  byte-identical without `--search`; c-testsuite inert at 220/220; corpus
  colorings unchanged item for item; a permanent per-project gate that
  `--search` >= no-search, whose reference is the IMPORTER rather than the
  coloring — the only test in the tree that can catch a false Red.
  (test/Project/search-false-red.cpp, test/Project/coloring-field-indirect.c,
  test/Project/item-graph-field-indirect.c,
  test/RealWorld/Cpp/search-never-worse.cpp)

- [x] FR-51 Library crates: distinguish Rust `lib` from `bin` (W5.11).
  `--emit=crate` hard-failed without a `c_main`, so EVERY crate this tool
  ever emitted was a BINARY crate. Two consequences, the second much larger:
   1. `--incremental` is designed to drop unportable items, but when the
      dropped item was `main` the emitter then hard-failed and the user got
      no crate, no `PORTING.md`, no per-item accounting -- the exact
      all-or-nothing behaviour `--incremental` exists to remove.
   2. LIBRARY PROJECTS were unreachable under any flag, which is most real C
      and C++ code. This was INVISIBLE in the corpus precisely because every
      corpus program was authored with a `main` so the differential oracle
      would have something to run -- a blind spot built into the benchmark's
      own design, not into the translator.
  Landed: `--crate-type=auto|bin|lib` (default `auto`), lib iff no `c_main`.
  Forcing `bin` without one is a located error rather than a crate that
  cannot link; forcing `lib` on a module with `main` demotes `c_main` to an
  ordinary exported function. A lib writes `src/lib.rs` and a manifest
  carrying an explicit `[lib]` (cargo would infer both; writing them means
  reading the manifest alone answers what kind of crate it is).
  `--emit=rust` now prints the crate root whichever shape it is, making that
  invariant total where it previously held only for input with a `main`.
  **Visibility, the load-bearing decision.** Exported in lib mode:
  functions whose emitted symbol carries no internal-linkage marker, and ALL
  record/enum definitions unconditionally -- types must be exported because
  Rust's private-in-public rule (E0446) requires any type named in an
  exported signature to be exported, and a C record carries no linkage of
  its own to leak. Globals are NEVER exported: module-level mutable state is
  not a usable Rust API, and global names are not a sound linkage oracle
  anyway (a function-local `static` is mangled `<function>_<name>` and
  inherits its ENCLOSING FUNCTION's tag, not one of its own).
  Internal linkage is NOT in the IR, which was checked first:
  `emitrust.func`/`emitrust.global` carry no visibility attribute, and
  MLIR's `sym_visibility` is already spoken for with an INVERTED meaning --
  `private` marks a body-less DECLARATION, so a file-`static` definition is
  non-private while an `extern` prototype IS private. The one surviving
  trace is the `tu<N>_` mangle, which survives because it must (two TUs may
  each define `static helper` and both land in one flat Rust module), so
  `isInternalLinkageSymbolName` reads back a fact the importer deliberately
  wrote down. Its one imprecision -- an identifier literally spelled
  `tu0_x` -- fails CONSERVATIVELY: under-export, never mis-export.
  **New harness outcome `LIB_BUILT`, deliberately not a widened
  TRANSPILED.** TRANSPILED is a fact about SEMANTICS (built AND ran AND
  byte-matched a native build); a library has no entry point, and `clang++`
  cannot even link an oracle from sources with no `main`. `LIB_BUILT` claims
  translatability and type-correctness only -- such a project could compute
  entirely wrong answers. It ranks between REJECTED and TRANSPILED, and
  TRANSPILED -> LIB_BUILT is a REGRESSION, not a lateral move. Conflating
  the two would have silently weakened the differential oracle that is this
  project's entire soundness argument.
  Also fixed a real bug FR-51 made reachable: `declLedgerName` reported a
  rejected decl by its C spelling, but the ledger symbol is a JOIN KEY
  against item-graph node keys and the importer renames `main` -> `c_main`.
  Only the DROP path was affected (a stub overwrites the symbol with its
  emitted name, which is why `polygon`/`shapes` already read `c_main`
  stubbed), and a dropped `main` previously produced no report at all.
  Gates: byte-identical `Cargo.toml`, `src/main.rs` and exit status for all
  103 pre-existing EndToEnd inputs replayed through their OWN `--emit=crate`
  RUN lines; c-testsuite inert at 220/220. **`argv-echo` -- the one corpus
  project that yielded nothing at any revision -- now emits a compiling lib
  crate with a report** (1 item, `c_main` dropped `[argv]`). New corpus
  project `ringbuf-lib` (3 TUs, 2 headers, no `main`, plus a deliberate
  file-`static` so both sides of the visibility rule are covered) scores
  LIB_BUILT 12/12, with a separate consumer crate linking against it and
  matching the native build byte for byte.
  (test/EndToEnd/lib-crate-external-caller.c,
  test/RealWorld/Cpp/Inputs/ringbuf-lib/)

- [x] FR-52 Traits for external requirements (W5.12). An undefined external
  is a REQUIREMENT ON THE ENVIRONMENT, not an error. The crate now declares a
  `pub trait Externals` whose ASSOCIATED FUNCTIONS are the unresolved
  externals, and the transitive closure of their callers becomes generic over
  it (`fn scaled<E: Externals>(..)` calling `E::host_scale(..)`). Associated
  functions rather than `self`-receiver methods: nothing to thread through
  signatures that have no receiver in C, monomorphises to a direct call, no
  `dyn`, no `unsafe`, no `extern "C"`. Propagation is a fixpoint over callers
  -- the smallest correct set, since a Rust caller can name an `E` only if it
  has one -- and converges on recursion.
  Architecture: the importer records the FACT
  (`emitrust.external_requirement` on the kept body-less `func.func`); a new
  pass does the work AFTER `convert-to-emitrust`, because the answer is
  expressed in emitted names and opaque string callees -- before conversion a
  `func.call` callee must resolve in the symbol table, so the requirement
  could not be erased at all. A module reaching the emitter with the marker
  still set fails exactly as an unresolved external always did, so forgetting
  the pass cannot silently emit a broken crate. The non-obvious third case: a
  function ADDRESS `Some(f)` is not a symbol use, so without handling it the
  generic function would be renamed out from under its own fn-pointer
  constant.
  Decisions, each refused rather than half-supported: GLOBALS keep rejecting
  (an associated const is a VALUE; a C `extern int` denotes mutable STORAGE
  WITH AN ADDRESS, and no associated trait item yields a place an assignment
  can write to). VARIADICS never reach the decision -- a body-less variadic
  is not imported at all -- and their call/address sites reject with
  locations on the C construct rather than on a whole-program fact. BIN
  crates stay an error: `fn main` is not generic and has no caller to
  instantiate it, and a `todo!()` default impl would turn a compile-time
  failure into a runtime panic AND let FR-44 score an unrunnable crate as
  fully ported, blinding the differential oracle. Name clashes are refused,
  not worked around: a project item named `Externals`, or `E` (a type
  parameter SHADOWS a same-named type inside the generic item, so a
  signature would quietly change meaning).
  NON-GOAL held: no binary library. `[lib]` still carries only `name` and
  `path`, so the emitted library remains a plain Rust `rlib`.
  Gates: byte-identical `Cargo.toml`, crate root, `PORTING.md`,
  `emitrust-progress.json` and exit status for all 104 pre-existing EndToEnd
  inputs; c-testsuite inert at 220/220. **None of the five pinned
  undefined-extern tests changed** -- evidence the scoping rule is
  conservative rather than convenient. New corpus project `extern-plugin`
  (the corpus's first OPEN project) scores LIB_BUILT 7/7;
  `test/Project/search-backtrack.cpp` ports 4/4 with 0 stubbed under
  `--crate-type=lib`, against the search baseline's 4/5 with 1 stubbed --
  that stub was the dropped external, now a declared requirement.
  (test/Import/C/multi-tu-external-requirement.c,
  test/Conversion/LowerExternalRequirements/, test/Target/Rust/trait-def.mlir,
  test/EndToEnd/lib-crate-externals-trait.c,
  test/RealWorld/Cpp/Inputs/extern-plugin/)
- [ ] FR-56 Compiler-shim front end (`emitrust-clang`). The build system, not
  a compilation database, drives per-TU work: `make CC=emitrust-clang` on an
  unmodified project must complete. The shim parses the FULL argv with
  clang's own `clang::driver::Driver` (never a hand-rolled filter), classifies
  arguments by whether they reach the `-cc1` frontend job (the principled
  definition of "affects the Rust result"; target/ABI flags such as
  `-target`, `-m32`, `-fshort-enums`, `-fpack-struct` DO affect it because
  type layout feeds the lowerings), DELEGATES the real compile to the real
  clang so `.o` files, configure/`cc-option` probes, version checks, and the
  final native link all behave, and side-emits the FR-57 per-TU artifact.
  Workflow fidelity is part of the contract: `@response-files`, `-MD/-MMD`
  depfiles, `-x`, `-E`/`-S` pass-through, exit-code and stderr transparency.
  Acceptance: a small real Makefile project builds unmodified with
  CC=emitrust-clang, producing byte-identical binaries to a plain-clang
  build, with one artifact per compiled TU.
  SPIKE (GO): `tools/emitrust-clang` proves the shape end-to-end -- a 3-TU
  `-O2 -Wall -MD` Makefile project builds byte-identical under the shim with
  one text-MLIR artifact per TU, `-D` macros provably reach the import,
  `--version`/`-E`/depfiles/response-files/compile-error exit codes pass
  through, incremental make recompiles exactly the touched TU, and a
  rejecting construct (volatile) is recovered without failing the build
  (test/Driver/emitrust-clang-shim.c pins the contract). Clang-21 driver
  notes: DiagnosticsEngine takes DiagnosticOptions by reference now;
  classification must run under an IgnoringDiagConsumer and only for `-c`
  lines (BuildCompilation PRINTS immediate args like --version). Remaining
  for the checkbox: cc1-key canonicalization (workflow args like
  `-dependency-file`/`-o` currently perturb the hash), forwarding target/ABI
  flags into the import, import-crash isolation (fork or post-hoc), argv0
  `cc`/`gcc` aliasing, and the FR-57 artifact format.
- [ ] FR-57 Per-TU artifacts ("object files as parse caches"). `-c` imports
  ONE translation unit in isolation and serializes the result: the imported
  emitrust module as MLIR bytecode plus the TU's item-graph shard and
  rejection-ledger entries, keyed by a hash of the canonicalized `-cc1` line
  (workflow-only argument changes MUST NOT change the key; a semantic or
  layout flag change MUST). The artifact is embedded in a `.emitrust` ELF
  section of the genuine object file (the gllvm model), so `ar` archives and
  existing link lines carry it with no build-system cooperation; a sidecar
  file is the fallback for non-ELF targets. Acceptance: bytecode round-trips
  (emit -> reload -> translate is byte-identical to the direct path), and the
  artifact survives `ar` + `ld` collection.
  LANDED (FR-57b, in the FR-56 shim): the canonicalized cache key and the
  embedded bytecode artifact. The shim hashes the cc1 line only after
  stripping the empirically measured workflow-noise blacklist (SPIKE 2
  below) plus the positional input path (tools/emitrust-clang/Cc1Key.h), and
  logs the FR-57 key PAIR — `cc1-key` (canonicalized command line) and
  `src-hash` (content bytes; widened from the main file to the whole
  preprocessed input by FR-57c below). The artifact is now MLIR
  bytecode (`<object>.emitrust.mlirbc`, the sidecar kept as the non-ELF
  fallback) and is embedded into the genuine object as a non-alloc
  `.emitrust` section via LLVM's objcopy-as-a-library, staged-and-renamed so
  no failure can truncate the `.o`; embedding failure warns and keeps the
  sidecar, never the exit code. Pinned: an `-o`/depfile rename keeps the
  key, a macro change misses it, the same content at another path keeps
  both halves, the section payload dumps back byte-identical to the sidecar
  and round-trips through emitrust-opt, and stripping `.emitrust` restores
  delegation byte-identity (test/Driver/emitrust-clang-shim.c). Still open
  for the checkbox: item-graph shard + rejection-ledger entries in the
  artifact.
  LANDED (FR-57c, header-aware src-hash): `src-hash` now covers the WHOLE
  preprocessed input, not just the main file's bytes, so a header-only edit
  misses the cache key. SPIKE verdict, measured: parsing the depfile the
  build itself produced is FRAGILE — it exists only when the build asked
  for one, and `-MT 'custom target'` puts caller-controlled text (spaces,
  potentially colons) before the `:` — while driving dependency collection
  internally is uniform; since the shim already links clang's frontend, the
  robust form needs no subprocess and no depfile syntax at all: the job's
  own cc1 line is replayed in-process through a `PreprocessOnlyAction` with
  a `DependencyCollector` attached (user headers only, the `-MMD` set;
  system-header CONTENT is deliberately out of scope because the system
  include PATHS are already in the cc1-key half, and under nix a toolchain
  change is a store-path change that misses there), with the
  dependency-output and output-file options cleared so the scan can touch
  neither the build's real depfile nor the just-produced object
  (tools/emitrust-clang/DepScan.h). The hash is an order-independent
  MULTISET of per-file content digests: content-only, so an mtime touch
  keeps it; path-free, so the same sources at another location keep it.
  Failure direction: a scan failure or an unreadable dependency warns and
  emits NO artifact — never a key that missed a dependency and could later
  be a wrong cache hit — with the delegated compile's outcome untouched;
  forced by the test-only `EMITRUST_TEST_UNREADABLE_DEP` env hook, since no
  real build can delete a header between the delegated compile and the
  hash in one invocation. Pinned: header content edit misses the key,
  mtime-only touch keeps it, `-MD -MF <renamed> -MT 'custom target'` keeps
  BOTH halves, a depfile-less compile hashes identically to a depfile one,
  and the unreadable-dependency path keeps the object and drops only the
  artifact (test/Driver/emitrust-clang-src-hash.c).
  LANDED (FR-57a, the FR-58-spike blocker): `--defer-externals` /
  `ImportOptions.deferExternals` import mode. An extern global or called
  function whose definition lives in another TU no longer rejects the solo
  import: it becomes a declaration-only `emitrust.global` / body-less
  `emitrust.func` carrying `emitrust.extern_decl`, and the Rust emitter
  REFUSES any module still carrying the marker ("unresolved deferred
  external ... must be linked against the defining translation unit"), so
  the flag cannot silently emit a broken crate. The shim imports in this
  mode (plus recover), so every cross-TU-referencing TU now yields an
  artifact. Defer takes precedence over the FR-52 trait policy; non-defer
  behavior is unchanged (the historical located rejection is regression-
  pinned). (test/Import/C/defer-externals.c,
  test/Driver/emitrust-clang-shim.c, test/Dialect/EmitRust/ops.mlir)
  Merge-oracle snapshot on `multi-tu.c` after FR-57a: solo shard 0 carries
  `@SHARED_COUNTER {extern_decl}` + body-less `@add`/`@lib_transform`
  obligations that shard 1 defines -- resolution is
  declaration-for-definition replacement -- and BOTH solos claim
  `@tu0_scale` for their own file-static, confirming the merge must
  alpha-rename per-shard tags to global ordinals.
  SPIKE (GO): import -> `--emit-bytecode` -> reload -> translate is
  byte-identical to the direct path on 112/112 single-TU EndToEnd inputs
  (bytecode ~2.5x smaller than text); an objcopy `.emitrust` section on a
  real `.o` links, survives an `ar` archive, and dumps back byte-identical,
  and the extracted payload still translates. First finding fixed on the
  spot: `!emitrust.array` of enum printed but did not re-parse
  (`ArrayType::isValidElementType` omitted EnumType;
  test/Dialect/EmitRust/types.mlir now pins the round-trip).
- [ ] FR-58 Link-step whole-program aggregation. "Linking" extracts every
  `.emitrust` payload from the link line's objects and archives, MERGES the
  item-graph shards (order-independent, deterministic), runs the FR-41
  3-color admission GLOBALLY, and materializes Rust for admitted items FROM
  THE CACHED MODULES -- no `.c` is re-parsed at link time. Cross-TU symbol
  unification and shape-dedup reuse the FR-26/FR-40 machinery but must hold
  at 10^3..10^4 TUs; collision scans and impl lookups become indexed, not
  linear. Acceptance: a multi-TU project built via FR-56 shim + FR-58 link
  emits a crate byte-identical to today's single-invocation
  `emitrust-cc --emit=crate` on the same sources.
  LANDED (FR-58 slice 1): `emitrust-cc --link a.o b.o -o out.crate
  [--build]` -- extract every shard (`.emitrust` section via
  llvm::object, `<obj>.emitrust.mlirbc` sidecar fallback, or a `.mlirbc`
  named directly), parse the bytecode, merge per the SPIKE-3 algorithm
  (tools/emitrust-cc/LinkMerge.h), and feed the merged module to the
  EXISTING --emit=rust/crate path with no C re-parse and no pass run.
  Prerequisite landed with it: the shim's solo import (importC,
  defer-externals mode) now tags file-statics with the TU-local
  placeholder `tu0_`, since nothing else in the IR records linkage and the
  merge must alpha-rename identically spelled statics apart; non-defer
  single-file imports are byte-identical to before. Pinned: the 2-TU
  shared-header program (cross-TU call, extern global, dedup'd
  struct/enum, colliding file-statics) links into a crate whose stdout
  matches the clang-built native binary AND whose crate root is
  BYTE-IDENTICAL to the joint import's, from the objects and from the
  sidecars alike (test/EndToEnd/link-merge-e2e.c); the undefined-symbol
  and shape-conflict link errors are located diagnostics
  (test/Driver/link-merge-errors.c); the shard tag is pinned in
  test/Driver/emitrust-clang-shim.c. Shape equality is print-to-string for
  now (OperationEquivalence is the upgrade). Still open for the checkbox:
  `ar` archive members, selective re-import of fact-starved items (extern
  pointer globals, SPIKE 2's constraint), owner-planning
  `soleTranslationUnit` divergence, indexed collision scans at 10^3+ TUs,
  and FR-59 workspace partitioning.
  SPIKE (design constraints found, all three from one probe -- importing the
  two `multi-tu.c` TUs solo vs jointly): (1) BLOCKER: a solo import of a TU
  referencing an extern global defined in ANOTHER TU hard-fails even under
  `--recover` and emits NO artifact (FR-52 traits cover extern functions
  only; globals deliberately reject) -- at kernel scale that is nearly every
  TU, so FR-57 needs a deferred-externals import mode where an undefined
  extern global becomes a link-time obligation recorded in the shard, not an
  import-time rejection; (2) per-TU tags are a LINK-time decision -- the
  joint import assigns `tu0_scale`/`tu1_scale` by project order, while each
  solo import would claim `tu0_` for itself, so shards must carry a
  TU-local placeholder tag that the merge rewrites (or tags keyed by content
  hash, not ordinal); (3) shape-dedup'd struct/enum defs appear in every
  shard that uses them and the merge dedups by the existing FR-26 shape key.
  Naive per-TU import + concatenation is NOT the joint import; the merge is
  a real pass with its own byte-identity oracle.
  SPIKE 2 (three implementation-detail results): (1) GO -- a MECHANICAL
  merge (drop `extern_decl` ops, alpha-rename shard-local `tu0_` to global
  ordinals, concatenate bodies in TU order) reproduces the joint import's
  Rust BYTE-IDENTICALLY on multi-tu.c, so the merge core is simple; (2) the
  FR-57 cc1 cache key's workflow-noise blacklist was enumerated
  empirically: the `-o` output, `-dependency-file`, `-MT`,
  `-sys-header-deps`, `-fdebug-compilation-dir=`,
  `-fcoverage-compilation-dir=` (and `-main-file-name` defensively) must be
  stripped before hashing, the input CONTENT hashed separately (via the
  depfile's file list); `-D`, the target triple, CPU, and the
  internal-isystem set correctly perturb the key; (3) CONSTRAINT -- an
  `extern` POINTER global cannot type its declaration stub solo (the
  base+cursor decomposition needs the defining TU's shape): defer+recover
  degrades gracefully (artifact emitted; accessing items stubbed/dropped
  and ledgered), but those items are recoverable ONLY with whole-program
  facts, so FR-58 is merge + SELECTIVE RE-IMPORT of fact-starved items
  (the ledger identifies them), not pure concatenation.
  LANDED (slice 2): static ARCHIVE inputs -- a `.a` on the link line
  (detected by magic, not extension) expands its members in archive order,
  reusing the object payload extraction; a member without a `.emitrust`
  payload warns and is skipped (real builds archive hand-written-asm
  objects). Archive-vs-loose linking is byte-identical. Shape-conflict
  detection upgraded from print-equality to
  `OperationEquivalence::isEquivalentTo` (IgnoreLocations) with the same
  located error contract; same-layout different-field-name structs conflict,
  byte-identical defs dedup silently. (test/Driver/link-merge-archive.c,
  link-merge-errors.c) Still open: selective re-import of fact-starved
  items, the owner-planning soleTranslationUnit divergence, indexed scans
  at 10^3+ TUs.
  SPIKE 3 (GO -- the merge algorithm is now fully experiment-specified): on
  a shared-header project where BOTH shards carry identical
  `struct_def @Point` / `enum_def @Mode`, the mechanical merge extended
  with first-occurrence type dedup (by symbol; a same-symbol
  different-shape pair is the link error) is again BYTE-IDENTICAL to the
  joint import. Algorithm: shard bodies in link-line order; drop an
  `extern_decl` op when some shard defines the symbol (error otherwise --
  the undefined-symbol link error); dedup struct_def/enum_def/global by
  symbol, first occurrence wins, shape conflict errors; alpha-rename
  shard-local `tu<N>_` tags to global ordinals; concatenate; verify; feed
  the existing crate pipeline.
- [ ] FR-59 Workspace partitioning (multi-crate output). One crate cannot
  hold a kernel-scale project. The link step partitions the item graph into a
  Cargo WORKSPACE of crates (per source directory/subsystem by default,
  overridable), with strongly-connected components CONDENSED so no
  inter-crate cycle exists (an SCC lands whole in one crate); cross-crate
  references become `pub` items behind the FR-51 export rules. Acceptance: a
  project with two acyclic subsystems emits two crates that `cargo build`
  together; a deliberately cyclic pair condenses into one crate rather than
  failing.
- [ ] FR-60 Kernel-corpus ratchet. The validation story at scale: byte-diff
  does not exist for a kernel, so the measure is the ADMITTED-ITEM ratchet --
  a per-project manifest (the c-testsuite ledger generalized) recording which
  items are green; CI fails on any shrink. First corpus: a Linux
  `allnoconfig` build driven by `make CC=emitrust-clang` where the SHIM
  passes (FR-56 workflow fidelity proven at scale) even while most items
  reject; the rejection ledger aggregates into a queryable per-construct
  report (inline asm, volatile, container_of, attributes) that ranks what
  semantic work buys the most frontier.
- [x] FR-61 Rustacean-style emission. The emitted Rust should read as
  expression-oriented Rust, not statement-per-op SSA transliteration; every
  slice is a pure EMITTER rendering change whose oracle is the EndToEnd
  byte-diff (behavior identical; golden-text churn expected and updated per
  slice). Corpus-measured opportunity (473 functions): ALL FIVE SLICES
  LANDED 2026-08-03 (61c's conversion-time while-lift closed it); final
  corpus: `let ` 10273 -> 3175 (-69%), `loop {` 222 -> 44 (178 genuine
  `while`s), tail expressions total, named locals + params, unsafe 0,
  byte-diff oracle held through every slice.
  - [x] 61a Tail-expression returns: a function-final `return v;` renders as
    the tail expression `v`, and when `v` is a single-use binding defined by
    the immediately preceding `let v = <expr>;` the pair folds to the tail
    `<expr>` (115 corpus sites). `return` in non-tail positions stays.
  - [x] 61b If-expression bindings: a deferred `let x: T;` immediately
    followed by an `if` whose BOTH arms end with their only `x = ...;`
    assignment renders as `let x: T = if c { ...; a } else { ...; b };`
    (24 corpus sites; the deferredInits analysis already identifies exactly
    these bindings).
    LANDED 61a+61b: emitter-rendered `return` statements are ZERO
    corpus-wide (every residual `return` lives in verbatim runtime shims);
    61b fires at 248 sites across 66 of 95 emitted crates. A dialect fact
    discovered en route: `emitrust.return` is a FuncOp-parented terminator,
    so a non-tail `return` cannot exist in the dialect -- the CF lift
    already funnels early exits to a single exit -- making 61a total, not
    partial. Byte-diff oracle held untouched; unsafe count 0 across all 95
    crates. (test/Target/Rust/tail-expr.mlir, control-flow.mlir 61b cases,
    plus expectation churn across the Target/Driver goldens, every line
    explained by 61a/61b.)
  - [x] 61c While-lift: `loop { ...; if c { break } ... }` shapes back to
    `while`/`while let` where the SCF lowering's shape allows (172 corpus
    `loop {`s; SPIKE FIRST -- the condition prefix is statements, not an
    expression, so only a prefix-free subset lifts mechanically).
    SPIKE 61c-0 (2026-08-03): NO-GO for the emitter-side mechanical lift;
    the premise is measured false. Census of ALL 303 `emitrust.loop`s
    (EndToEnd 195 + c-testsuite 108, post-61d/61e shapes, classified in
    `emitLoop` against the live inlinedOps/droppedOps sets so "prefix-free"
    means what the emitter actually prints):
      - bucket A (front `if c { break }`, prefix fully consumed,
        mechanically liftable): 0 -- the shape DOES NOT EXIST; the SCF
        lowering always renders the body-if BEFORE the exit test.
      - B_scf_tail_assigns (canonical destruction shape: cond binding,
        deferred carried lets, body-if with else-defaults, tail
        `if c == false { exit copies; break }`, backedge assigns):
        289 (95%). Not emitter-liftable: the exit copies and else-defaults
        read values scoped INSIDE the loop, and Rust's `while` has no
        exit-edge slot to put them.
      - B_scf_tail_plain (tail break-if, no exit copies): 14 (5%) -- all
        impure-condition loops (`while ((c = fgetc(f)) != EOF)`): the
        condition prefix contains calls, so idiomatic Rust for them IS
        `loop { .. break }`; no lift wanted.
      - C/D (front-break-with-assigns, non-if shapes, while-let): 0.
    The bucket-A prototype (structural `cond == false` negation fold,
    while-head from the 61d capture map, prefix consumed through
    emitDropOrCapture) is implemented and byte-inert on the whole corpus
    (443/443, EndToEnd 123/123) precisely because bucket A is empty --
    kept as the census instrument (EMITRUST_LOOP_CENSUS) and as the
    rendering skeleton for the real fix.
    DESIGN CONSTRAINT for a future slice: the lift belongs BEFORE
    SSA-destruction, not after. An `emitrust.while` op (condition region
    yielding i1 + body region) emitted at SCF-conversion time -- where the
    loop-carried values are still SSA and the exit copies do not exist
    yet -- would turn ~95% of today's loops (the B_scf_tail_assigns
    bucket) into genuine `while <cond> { .. }` renderings, with the
    impure-condition 5% legitimately staying `loop`. That is an
    importer/conversion FR, out of emitter scope.
    SPIKE 61c-1 (2026-08-03): GO. `emitrust.while` prototyped end to end
    (dialect op with condition region + `emitrust.condition` terminator;
    conversion-time lift in WhileLowering; emitter folds the condition
    chain into the head via the FR-61d capture machinery, Cond position).
    The REAL lift-cf-to-scf canonical form was discovered en route: the
    loop body lives in the BEFORE-region as a cond-guarded `scf.if` (exit
    defaults in its else-yield), the after-region is a forwarder -- so
    the lift recognizes {pure prefix; body-if; condition} and splits it:
    prefix -> condition region, then-arm + after ops -> body, else-yields
    -> the loop's replacement values. No result lets, no exit copies, no
    tail break. Conversion count: 201 of 304 corpus scf.whiles lift
    (66%); the 103 fallbacks are impure/entangled shapes that keep
    today's `loop` lowering. One liveness correction found by the E0384
    guard exactly as designed: `analyzeControl` needed an explicit
    WhileOp case (generic fallback missed write-recurrence ->
    `loopReassign`); with it, EndToEnd 123/123 byte-diff green with the
    lift firing at 201 sites. Slice lands with this spike.
    LANDED 61c (2026-08-03): `emitrust.while` (condition region +
    `emitrust.condition` terminator, body region; breaks legal inside),
    the conversion-time lift for both the simple and the canonical
    body-if shapes, and the emitter's fold-into-head rendering (a
    non-foldable condition op is a located error). 201 of 304 corpus
    scf.whiles lift; corpus `loop {` 222 -> 44, `while` 178, `let `
    3882 -> 3175 (exit copies, result lets, and condition bindings gone).
    The 103-loop remainder (impure conditions -- fgetc-style -- and
    entangled exit values) keeps `loop { .. break }` BY DESIGN: that is
    the idiomatic Rust for those shapes. Full suite 444/444, EndToEnd
    123/123 byte-diff green (one E0384 caught and fixed en route:
    analyzeControl's explicit WhileOp case), unsafe 0. Pinned in
    test/Conversion/SCFToEmitRust/while.mlir (lift + both NOT-lifted
    cases), Dialect round-trip + verifier rejections,
    Target/Rust/while-loop.mlir (head folding, `while true`, breaks),
    Target/Rust/errors.mlir (unfoldable condition), and the loops.c
    EndToEnd shape pin.
  - [x] 61d Expression-tree inlining: fold single-use scalar `let vN`
    temporaries into their one consumer where evaluation order provably
    cannot change (loads/pure ops only; SPIKE FIRST -- this is the largest
    readability lever and the most semantics-sensitive).
    LANDED 61d slice 1 (2026-08-03): full pure set (constants incl.
    bool/opaque with literal suffixes on numerics, add/sub/mul/div/rem,
    and/or/xor/shl/shr, scalar cmp, cast, bitcast, load, alias let) inlines
    at classified consumer positions via a mandatory-`ExprPos` emitOperand
    and ONE `needsParens` table; unused pure values drop with reverse-order
    cascades. Corpus: `let ` bindings 10273 -> 6535 (-36%), ~3079 inline
    sites + ~553 drops across the single-TU EndToEnd drivers; EndToEnd
    123/123 byte-diff green, full suite 440/440, emitted `unsafe` count 0.
    Two rustc GRAMMAR quirks found by the oracle and pinned in
    test/Target/Rust/inline-expr.mlir: text ending in a bare `as T` cast
    misparses as generic args when left of `<<`/`<` (tracked as a per-
    capture `endsInCast` bit -- rank alone cannot see it), and the
    subscript-index read idiom had to become exact (`lvalueIsMutated` on
    the place) once drops made it load-bearing (E0425 otherwise). A dropped
    unused div/rem elides a div-by-zero panic: C UB refinement, same
    direction as dead-store elision. Cast-to-bool stays undroppable so its
    rejection diagnostic survives. Not inlined by design: literals, selects,
    calls, global/cell reads (61d-2), for-bounds (name-lookup rendering),
    fn-ptr cmp/None. emitFor/emitGlobalCells/61b-arm bodies bypass capture
    (map-miss falls back to the name; drops ARE skipped there).
    LANDED 61d slice 2 (2026-08-03): constants inline POSITION-
    INDEPENDENTLY -- no same-block/barrier rule at any use count, every
    real use must be a classified consumer (one for-bound keeps the named
    binding for all uses) -- and a MULTI-use constant duplicates its
    suffixed literal at every site iff the text is <= 12 chars (measured:
    1098 surviving multi-use constant bindings, suffixed length p50=4
    p90=6; everything >12 was a pathological literal like
    `-9223372036854775808i64` or `4000000000.75f64` x52 uses that reads
    better named; 12 collapses 98%). `emitrust.global_load` joined the
    SINGLE-use inline set behind the same barrier wall (GlobalStore/
    CellSet/calls block; a store-consumed load nests the accessor closures
    -- pinned in globals.mlir); it stays out of the droppable set.
    `emitrust.cell_get` promotion SKIPPED: its defs live in
    `emitrust.global_cells` bodies, rendered by the capture-bypassing
    loop, so promotion would be inert -- revisit if that loop ever routes
    through emitBlockBody. Corpus: `let ` 6535 -> 4655 (-55% vs baseline
    10273), inline captures 3079 -> 4875, multi-use constant binding
    survivors 1098 -> 8 (long literals / for-bound consumers only),
    EndToEnd 123/123 byte-diff green, full suite 440/440, unsafe 0.
    LANDED 61d slice 3 (2026-08-03): the three capture-bypassing loops
    (61b arm bodies, for bodies, global_cells bodies) route through the
    shared drop/capture prelude (`emitDropOrCapture`), so the EXISTING
    qualification fires there -- arm tails inline into the never-parens
    Stmt position, for-body candidates into their consumers; and an
    if-expression binding whose consumed `if` immediately precedes the
    tail return with the return as its only READ folds via the FR-61a
    machinery (prologue suppressed in emitIfExprBinding, `;` erased at
    the return): the function body IS the if-expression --
    `fn sum_to(n: i32) -> i32 { if n <= 0i32 { 0i32 } else { let v6 =
    sum_to(n - 1i32); n + v6 } }` is the emitted corpus shape. Cell_get
    promotion is now UN-BLOCKED by the routing (deliberately not taken
    this slice). Corpus: `let ` 4655 -> 3882 (-62% vs baseline), inline
    captures 4875 -> 5576, EndToEnd 123/123 byte-diff green, full suite
    443/443, unsafe 0.
    SPIKE 61d-0 (2026-08-03): GO. Buffered-capture mechanism prototyped for
    Constant+Add only: capture the op's normal statement rendering from the
    emitter buffer, strip indent + `;\n`, suppress the let prologue while
    still calling assignName (numbering stability), re-print at the unique
    consumer with rank-based parens. 206 sites fired corpus-wide, EndToEnd
    123/123 byte-diff green, full suite 439/439, nested-region capture works
    for free (if/loop bodies flow through emitBlockBody). Corrections the
    implementation must carry: (1) computeInlineCandidates runs AFTER
    tail-fold candidate selection, not before; (2) non-finite float
    constants (path-expression rendering) are disqualified from suffixing;
    (3) the tree builds -DNDEBUG, so mechanism invariants must be
    emitOpError failures, not asserts; (4) emitFor/emitGlobalCells/
    emitArmBodyWithTail iterate ops without emitBlockBody -- captures there
    degrade gracefully to by-name, route or accept deliberately; (5) pin
    exponent-float suffixing (`1e30f64`) by test, absent from corpus.
  - [x] 61e Real C local names: `emitrust.variable` gains an optional
    `name` attr carrying the final Rust spelling (importer-side
    mangleMemberName); the emitter prefers it over vN, `_`-prefixes unused
    names, and uniquifies per function (never shadows). Parameters carried
    via an `emitrust.param_names` func attr. SCOPE LIMIT (design-verified):
    LANDED 61e slice 1 (2026-08-03): `emitrust.variable` carries
    `named "<spelling>"` (attr `c_name`, explicit assembly clause, verifier
    enforces identifier shape); the importer names the five decl-bound
    sites (emitLocalVar place path, fn-ptr holder, owner place after its C
    array, FILE* handle, by-value param shadow) through
    createVariablePlace's new trailing rustName -- all other VariableOp
    creations stay anonymous. Emitter binds under the carried spelling
    with the `_`-prefix rule (drop-aware) and a per-function
    usedBindingNames set: collisions uniquify `x`/`x_1`, the vN counter
    skips claimed spellings (a C local named `v1` can never collide), and
    `self`/`__emitrust_tl` are pre-seeded. Corpus: 237 of 388 variable ops
    named (61%); 441 named bindings in the emitted crates; EndToEnd
    123/123 byte-diff green (renames are behavior-inert; a collision bug
    would be a loud rustc E0428/E0425), full suite 442/442, unsafe 0.
    Golden churn: 53 Import goldens' variable lines gained `named "..."` +
    recover-stub-callable's RUST line (`local.callee(...)`);
    tests test/Dialect round-trip+invalid, test/Import/C/local-names.c,
    test/Target/Rust/variable-names.mlir pin the three layers.
    LANDED 61e slice 2 (2026-08-03): the importer sets a discardable
    `emitrust.param_names` ArrayAttr (one slot per SIGNATURE INPUT; empty
    for unnamed C params, by-value shadows, the receiver, and cursor
    inputs; string-cursor BASE slots are named so the slice reads back as
    `(*name)[..]`; omitted entirely when every slot is empty). The
    FuncToEmitRust discardable-attr forwarding carries it for free; the
    emitter claims each named argument through the same uniquifier as
    named locals (args first, so a same-named local becomes `name_1`;
    unused named params render `_name`; methods keep hard-wired `self`,
    slot 0 ignored; named params do not consume vN counter numbers).
    Va-clone signatures keep vN (clone path not wired -- synthesized
    extras dominate there). Corpus: 430 of 494 emitted params named
    (87%; remainder = shadowed by-value/cursor/unnamed/clone params);
    `fn sum_to(n: i32) -> i32` with `n` at every use is real. Full suite
    443/443, EndToEnd 123/123 byte-diff green, unsafe 0. FR-61 wrap-up
    corpus numbers: `let ` bindings 10273 (pre-61d) -> 4655 (-55%),
    ~4875 inline sites + ~553 drops, 441 named local bindings, 430 named
    params, byte-diff oracle held through every slice.
    plain signed non-address-taken scalar locals are dissolved by mem2reg
    and re-materialize as SCF-lowering lets with no decl association --
    they keep vN this iteration; naming them needs work at SCF-lift time
    (future FR). Named now: parameters, by-value param shadows, aggregates,
    unsigned scalars, enums, fn-ptrs, address-taken scalars, STL/FILE
    locals, owner structs.

**Measurement defect found while landing FR-52 (2026-07-30).** FR-52's report
contradicted a premise this document and the accompanying paper had asserted:
that six `multi-tu*` EndToEnd projects were rescued by FR-43's search. They
are not. The paper's experiment harness discovered every `test/EndToEnd` case
as a SINGLE translation unit, but those six are deliberately two-TU tests
whose companion source lives in `Inputs/` and is named on the test's own lit
`RUN:` line. Feeding half a two-TU project makes a symbol undefined that is in
fact defined, so the search was repairing damage the HARNESS had done. The
affected published numbers are E3's "9 projects improved" and E2's rescue
count; both are being re-measured with discovery derived from each test's RUN
line. Recorded here because the failure mode -- a benchmark harness that
silently mis-invokes the tool and then credits the tool with recovering --
is not specific to this project.

### Cherry-pick assessment: `verified_transpilation_pipeline`

The archived prototype (`~/src/archive/verified_transpilation_pipeline` on
`rainier`; a ~21 kloc Rust crate: libclang parser, own C AST, petgraph
graphs, Z3-backed verification conditions) is a PARALLEL architecture to
this one, so nothing lifts as code. Three of its ideas are load-bearing
here and are cherry-picked as design:

- Its `ir/graph.rs` `ConstraintDependencyGraph` (interprocedural dependency
  edges with cycle detection and topological order, indices rather than
  `Rc<RefCell>`) is the shape FR-40's item graph takes.
- Its `ir/transform.rs` `TransformationSpace` — several candidate Rust
  types per C construct, each with a cost, "lower cost = more idiomatic",
  selection ranked by cost — is FR-43's representation dimension. Its Z3
  validity check is NOT adopted: this project's soundness argument is
  differential execution against a native build, and adding a solver
  dependency to decide what a recovering import can answer by attempting
  the import is a worse trade.
- Its `analysis/integrated.rs` "OnlyWhenRequired" loop (start conservative,
  attempt compilation, refine from the errors, repeat to convergence or an
  iteration cap) is exactly FR-43's search loop, and its iteration cap is
  why FR-43 is bounded rather than exhaustive.

Its `examples/parse_compilation_database.rs` is the one directly portable
piece and becomes FR-45.

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
- [x] C99-8 long double: maps to f64 — the same type as double — as a
  UB-refinement (REVISED for CTS 00204; the former PERMANENT rejection
  is withdrawn). Rationale: C requires long double to be at least as
  wide as double, every f64-exact value round-trips the substitution
  unchanged, and the supported shapes (L-suffixed literals, copies,
  parameters/returns/struct members, printf %Lf-family output of
  f64-exact values) perform no extended-precision arithmetic whose
  extra bits a defined program could rely on — the substitution only
  narrows evaluation precision, latitude C's FLT_EVAL_METHOD model
  already grants in the other direction, and the 00204 differential
  oracle is byte-exact under it. Consequences: double <-> long double
  conversions are identities (no arith.extf/arith.truncf); L-suffixed
  literals (decimal and hex) convert their x87 APFloat to IEEE double
  at import, correctly rounded; printf's L length modifier is accepted
  on the floating conversions exactly like the unmodified twins (bare
  %Lf keeps the __emitrust_fmt_f64 fast path, adjusted forms route
  through __emitrust_fmt_float) and stays a located rejection on the
  integer conversions ("unsupported: length modifier 'L' on printf
  '%d'"). Constructs that would OBSERVE the substitution stay
  rejected: sizeof/_Alignof over long double ("unsupported:
  sizeof/alignof of long double" — the C fold would promise the
  16-byte x86-64 ABI slot the emitted 8-byte f64 never keeps, the same
  reasoning as the bit-field sizeof rejection) and the %La/%LA
  hex-float conversions ("unsupported printf format specifier '%La'" —
  hex-float output renders the BITS, and an x87 80-bit value and the
  substituted f64 print different mantissas even when f64-exact).
  (test/Import/C/long-double-f64.c, long-double-f64-invalid.c,
  printf-extended-invalid.c; exercised end-to-end by c-testsuite 00204
  and test/EndToEnd/varargs-monomorph.c)
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
  enum-typed global elements. Multi-dimensional arrays and their nested
  initializer lists ARE supported (each dimension renders its element list;
  see arrays-multidim tests).
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
- [x] C99-17 Flexible array members, AMENDED by CTS-BR (00216): a FAM
  (C99 6.7.2.1p16, `T tail[];`) is tolerated at the DECLARATION — the
  record imports with sizeof excluding the FAM, exactly C's sizeof. On
  a byte-region record a static FAM-tail initializer additionally
  folds into an EXTENDED byte image past sizeof; on a typed record the
  field is dropped (a non-empty typed FAM-tail constant stays
  rejected). GNU zero-length array members (`T r[0];`) get the same
  zero-size, field-less treatment. What remains rejected, with
  dedicated located wordings that never degrade to the generic
  "unsupported: non-constant array size" fallback, is RUNTIME access
  to the tail: "unsupported: flexible array member access" and
  "unsupported: zero-length array member access" at the access site.
  (test/Import/C/flexible-array-invalid.c; positive declaration-side
  pins in test/Import/C/byte-region-aggregates.c, typedfam and params
  splits; see the 00216 disposition below.)
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
  Variadic pointers, void*/data-pointer components, and arrays of
  function pointers are located rejections. Argument-carrying calls
  through prototype-less K&R pointers refine the callee decl via
  callsite-prototype inference when it traces to a local-storage
  parameter/local (FR-29, CTS 00209); non-decl-traceable callees keep
  the located no-prototype rejection.
  (test/Import/C/fn-pointers.c, fn-pointers-invalid.c,
  fnptr-noproto-infer.c, fnptr-noproto-infer-invalid.c,
  test/EndToEnd/fn-pointers.c, fnptr-noproto-infer.c,
  test/Target/Rust/fn-pointers.mlir)
- [x] C99-28 String literals as char-array initializers and as pointer
  values, with the C escape set (beyond the original printf-format-only
  support). Array initializers: `char s[N] = "..."` / `char s[] = "..."`
  for plain, signed, and unsigned char arrays at both scopes (bytes are
  bytes; the unsigned elements live in the ui8 domain). Block scope
  lowers to per-element byte assigns over the default-zero place — the
  literal's bytes plus the terminating NUL when it fits (C99 6.7.8p14),
  remaining elements keeping the zero fill; file scope folds through the
  C99-11/14 APValue path to a typed i8/ui8 ArrayAttr on
  `emitrust.global`. Embedded NULs in the literal are ordinary data;
  wide (`L"..."`) literals fill wchar_t (i32) arrays one code unit per
  element (CTS-L3). Pointer values: `char *p = "..."` (and const char *)
  binds a read-only string-literal region (CTS-P1) — an i64 cursor into
  an immutable const-marked backing byte array holding the bytes plus
  the terminating NUL. Expression positions complete the entry: a
  subscript directly on a literal (`"abc"[i]`, constant or variable
  index) and the deref-of-arithmetic spelling (`*("abc" + n)`) read
  through the same cached backing, created on demand for anonymous
  decayed literals; `sizeof("...")` folds to length + 1 through clang's
  layout query (adjacent-literal operands included); a literal passed to
  a user-defined function's char-pointer (slice) parameter materializes
  a fresh mutable backing per call (FR-28, writes being UB makes the
  copy unobservable); string-helper arguments (strlen, ...) slice the
  backing; printf/puts %s shapes are unchanged; a literal compared
  against a null pointer constant folds to false. Adjacent string
  literals concatenate before import (clang folds translation phase 6),
  so every position above accepts the joined form. The full C escape
  set — simple escapes including \a \b \f \v \r \?, octal, and hex —
  arrives from clang already decoded to bytes, so every path above is
  spelling-blind (character constants share this machinery, C99-4).
  Located rejections, each pinned: non-ASCII bytes in any literal path
  (the ASCII-only policy keeping printed contents exact through the
  %s/%c helpers — unchanged and deliberate); u8/u/U literal kinds
  everywhere and wide literals outside the array-initializer position
  (a wide subscript has no byte backing); every write into a literal —
  plain and compound assignment, ++/--, and the deref spelling all
  funnel through one store guard on the const-marked backing (writing a
  C string literal is UB); literal-to-literal == (pointer identity is
  unspecified in C; each literal is its own backing, so the
  same-object comparison rule rejects the pair); %s literal arguments
  with embedded NUL or non-printable bytes; a literal bound through an
  unsigned-char pointer cast (CTS-P3 cast traffic); and `__func__`
  element access (C99-29).
  (test/Import/C/strings.c, strings-exprs.c, strings-invalid.c,
  pointers-string-literal.c, globals-pointer-string.c, strings-wide.c,
  printf-slice-param.c, char-constants.c, test/EndToEnd/strings.c,
  strings-exprs.c, string-cursor.c, char-constants.c)
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
- [x] C99-37 Variadic function definitions and va_list: bounded va_list
  bodies MONOMORPHIZE per call site (REVISED for CTS 00204); everything
  the monomorphizer cannot see stays a located rejection. Rust has no
  stable variadic ABI or safe varargs access, so no va_list object ever
  survives into the emitted Rust; instead, a Pass-A planner
  (`planVaMonomorph`, before any declaration imports) claims every
  variadic DEFINITION whose body uses va_list and, when the body stays
  in the bounded shape, synthesizes one clone per distinct
  extras-signature over its direct call sites. Clone parameters are
  exactly (named parameters, that site's extra arguments BY VALUE in
  declared order) — no synthetic trailing parameter; the consumption
  cursor is an internal i64 local. Inside a clone va_start resets the
  cursor, va_end is a no-op, and each va_arg(ap, T) becomes a dispatch
  over the cursor selecting among the extras whose static type is T
  (the cursor increments per read); a cursor position with no matching
  extra — UB in the C call — is a deterministic panic. Struct-typed
  va_arg reads (the 00204 HFA shapes) fall out for free: extras are
  ordinary Copy values, so no calling-convention modeling is needed.
  Call sites rewrite to their clone; the original variadic symbol is
  never emitted, and an in-scope definition with zero call sites drops
  entirely. The bounded-shape scope checks are located rejections,
  raised BEFORE any callee prototype imports: "unsupported: va_copy"
  (a cloned cursor's lifetime is out of scope), "unsupported: va_list
  escapes variadic definition" (ap passed to ANY callee — the callee
  would consume varargs the monomorphizer cannot see; this beats the
  callee's own va_list-parameter type rejection), and "unsupported:
  address of variadic definition" (an escaping function address makes
  the call-site set non-enumerable). Outside variadic definitions the
  va_list TYPE keeps its rejection in every position — local,
  parameter, field, global — with the located "unsupported: va_list
  type", so a hand-rolled vprintf-style helper can never import as the
  target's register-save-area struct, and the v*printf family stays
  unreachable by construction. The older carve-outs stand unchanged:
  (1) the CTS-F1/CTS-P9 fixed-prototype import — a va_list-FREE
  variadic definition imports as its named parameters only and call
  sites drop effect-free extras (an extra with side effects is
  rejected); (2) printf-family CALL SITES route through the hosted
  printf/puts machinery (C99-47/48) when the project supplies no
  definition. (test/Import/C/varargs-monomorph.c,
  varargs-monomorph-invalid.c, varargs-def.c, varargs-def-invalid.c;
  differential test/EndToEnd/varargs-monomorph.c, varargs-def.c;
  exercised at scale by c-testsuite 00204 — 35 call sites, 33 distinct
  clone signatures, 14 struct-typed va_arg sites)
- [x] C99-38 Multiple translation units: several .c files are imported and
  merged into one flat crate with extern object and function resolution
  across units, and internal (static) linkage kept distinct by per-unit
  symbol mangling (a single flat module needs no pub/non-pub visibility).
  Undefined externs and conflicting external definitions are rejected with
  located diagnostics. (test/EndToEnd/multi-tu.c, test/Import/C/multi-tu.c,
  multi-tu-undefined-extern.c) See FR-26.
  W3.0 revision: a va_list-using variadic (C99-37) DEFINED in one TU and
  CALLED from another is a located rejection, not the silent breakage it
  was before. `planVaMonomorph` enumerates a va_list variadic's call
  sites WITHIN A SINGLE TU only (each TU is its own `clang::ASTContext`,
  so a caller in another TU is invisible to it), and a va_list-monomorphized
  definition is ALWAYS replaced by its per-site clones — it never keeps an
  ordinary symbol of its own name. Left alone, a cross-TU call site would
  reference a symbol that exists nowhere in the merged module: an
  unresolved call escaping with no located diagnostic. `importCProject` now
  pre-scans every parsed AST (`collectCrossTuVaListVariadics`, before any
  TU imports, so the scan is independent of which TU — caller or definer —
  appears first on the command line) for externally visible va_list-using
  variadic definitions and records their symbol names; `emitCall` consults
  this registry only when a variadic callee's definition is invisible in
  the current TU, raising "unsupported: call to a variadic function
  '<name>' defined in another translation unit". A NON-va_list
  (fixed-prototype, CTS-P9) variadic called cross-TU is unaffected: it
  keeps the historical generic "unsupported: call to a variadic function"
  wording (it was already rejected before this revision — cross-TU
  resolution for it is not yet attempted at all). A variadic defined AND
  called within the SAME TU is unaffected and still monomorphizes.

  W3.5 (real cross-TU va_list monomorphization) DEFERRED — the located W3.0
  rejection stands. A spike built the whole-program enumeration cleanly (a
  pre-pass over every AST, after `crossTuVaListVariadicNames` is complete,
  deduplicates each cross-TU all-scalar extras signature into a shared clone
  plan named `<symbol>__ctN`, so the defining TU and every calling TU agree
  on the clone name with no coordination) and definer-first import orders
  worked end to end. It foundered on the caller-BEFORE-definer order, which
  the differential requires (`emitrust-cc main.c def.c` imports the caller
  first): a monomorphization clone is a SYNTHESIZED symbol with no C
  prototype, so a caller TU imported before the definer has nothing to
  forward-declare it from, and `emitCall` rejects the reference to the
  not-yet-materialized `@<symbol>__ctN`. The three escape routes each cost
  more than the feature is worth: (i) reordering the import loop so definers
  precede callers perturbs the module's function EMISSION order (functions
  emit in import order — verified: reversing an existing multi-TU pair
  reorders the output), breaking every multi-TU byte-snapshot; (ii)
  pre-declaring the clone in the caller is impossible because the clone's
  signature (e.g. the `fmt` param's `&mut [i8]` cursor classification) is
  derivable ONLY from the definition's body, which lives in another
  `ASTContext` the caller cannot reach; (iii) an eager clone-materialization
  pass before the main loop needs the defining TU's full Pass-A, which the
  main loop then re-runs, risking double-planning on the completed 220/220 C
  ledger. Thin demand (a cross-TU va_list variadic is rare) plus this
  disproportionate, C-path-risking plumbing put it on the same
  conservative-but-sound footing as the G2/G7 retentions; reopening it is a
  dedicated future wave that would first refactor clone materialization to be
  decoupled from the defining TU's live import state. (test/Import/C/
  multi-tu-varargs.c, Inputs/multi-tu-varargs-def.c)
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
  PARTIALLY RESOLVED for one closed subset: a self-referential pointer
  member of a promoted owner-struct array (`struct uf_node *parent`
  pointing at another element of the SAME array) is no longer a blanket
  rejection — see FR-37/FR-38 (`planArrayMemberPointers`, an
  `emitrust.enum_def` with one variant per array index, decoded/written
  via a genuine `match`). This resolves the member-pointer half of the
  shape; the RETURNED-pointer half is resolved only for the array-rooted
  case, where every return site provably roots in the same owner class as
  a data-pointer parameter of the same method (FR-36's owner-index
  return, Stage 1) — general arbitrary pointer returns (a pointer into a
  callee-local, non-array-backed object) remain rejected. Confirmed by
  direct test: `test/RealWorld/Inputs/binary-tree.c`'s `insert`, which
  returns a pointer to a freshly `malloc`'d node rather than a cursor into
  a caller-visible array, still rejects with "unsupported: returned
  pointer value (only a returned function address has a representation;
  a cursor into a callee-local region would dangle)" — the blocker there
  is dynamic memory (C99-46, out of scope for this plan), not the
  member-pointer or owner-index-return mechanism, and is left untouched.
- [x] C99-44 Unions. DECIDED and SHIPPED: the one-slot struct model —
  a supported subset with documented located rejections, not an enum
  mapping and not a blanket rejection. A named or untagged union
  RecordDecl imports as a ONE-FIELD struct whose storage field is the
  slot arm's leaf (name and type), generalizing the CTS-R2
  anonymous-union slot machinery — every arm's spelling aliases that
  slot, so no non-slot arm name reaches the IR. The slot is the FIRST
  arm, except in the byte-array mix (CTS-R3 T1.1) where it is the first
  non-array arm. SUPPORTED ARM MATRIX: arms that all map to one
  identical type (exact by C11 6.5.2.3); same-width integer arms
  differing only in signedness (accesses through the differently-signed
  arm wrap a bit-exact `emitrust.cast` reinterpret, reads slot->arm and
  stores arm->slot); a float arm paired with a same-width integer —
  float/32-bit and double/64-bit — whose accesses wrap an
  `emitrust.bitcast` (a dedicated dialect op rendered as Rust
  to_bits/from_bits, bit-exact by definition, deliberately distinct
  from the value-converting `as` of `emitrust.cast`); equal-total-width
  constant integer-array arms over an integer slot (type-level
  admission only, every access through the array arm rejects at the
  access site); single-arm unions; unions as struct members; union
  globals with constant initializers (the initializer lands on the
  slot; a float-pun arm's constant crosses the domain at compile time
  as its exact bit pattern); designated local initializers through pun
  arms (place takes the slot's type, value reinterprets onto it);
  compound assignment, increment/decrement, and value-position
  assignment through pun arms (load reinterprets slot->arm, the
  computation runs at the arm's type, the store reinterprets back).
  REJECTION MATRIX, all located: bit-field arms, unnamed/anonymous
  arms, and pointer arms (`unsupported: union with a ... arm`); empty
  unions (`unsupported: union with no members`); scalar arms of
  differing sizes — int/int, float/int, double/float
  (`unsupported: union arms of differing sizes`); aggregate or enum
  arms not identical to the slot's type
  (`unsupported: union arm cannot alias the storage slot`); access
  through a byte-array arm
  (`unsupported: union byte-array arm access`); taking the address of
  any union member (`unsupported: taking the address of a union
  member`); ++/-- through a float pun arm (the general non-integer
  ++/-- rejection, reached because the load reinterprets to the arm
  type first). Anonymous union MEMBERS (CTS-R2) keep the stricter
  identical-leaf-only admission — no puns through anonymous arms.
  Traceability: dialect `include/EmitRust/EmitRustOps.td` +
  `lib/EmitRust/EmitRustOps.cpp` (`emitrust.bitcast`, verifier:
  exactly one f32/f64 side and one same-width integer side) and
  `lib/Target/Rust/TranslateToRust.cpp` (`emitBitcast`); importer
  `lib/ImportC/ImportC.cpp` (`collectUnionSlot`,
  `flattenedFieldStorage`, `reinterpretScalarBits`,
  `reinterpretUnionArmRead`/`reinterpretUnionArmWrite`, pun hooks in
  `emitRecordInitField`, `emitCompoundAssignToPlace`,
  `emitIncDecValue`, `emitBinaryRValue`, cross-domain constants in
  `convertAnonymousSlotInit`, union routing in
  `mapType`/`importRecord`/`convertAPValueInit`); tests
  test/Dialect/EmitRust/ops.mlir + invalid.mlir (bitcast roundtrip and
  verifier pins), test/Target/Rust/arith.mlir (to_bits/from_bits
  rendering incl. signless `as` wrapping), test/Import/C/unions.c
  (alias, single-arm, struct member, global initializer, signedness
  pun, untagged local, float pun both slot orders, double pun,
  designated pun-arm local init — the last also pins the fixed
  arm-typed-store-into-slot-field defect),
  test/Import/C/unions-invalid.c (int size mismatch, float/long,
  double/float, aggregate arm, pointer arm, bit-field arm, empty
  union, float-arm ++ — all located; supersedes the deleted
  test/Import/C/union.c whose int/float rejection pin the float pun
  made stale), test/Import/C/union-bytearray-arm.c + -invalid.c,
  test/EndToEnd/unions.c (differential: both pun families in both
  directions, globals, designated inits, compound assign/++/value
  position), c-testsuite 00042.c, 00210.c, 00218.c (see CTS-R3).
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
- [~] C99-46 Dynamic memory: malloc, calloc, realloc, free. Stage 1
  (W4.2e, FR-39): a LOCAL const-size flat buffer (`T *p =
  malloc(cap*sizeof(T)); p[i] = ...; free(p);`) synthesizes a mutable
  `[T; CAP]` backing + cursor (Part A); a fixed-CAP node pool with index
  handles (`malloc(sizeof(struct T))` in a foldable-trip-count loop
  building a self-referential linked structure) synthesizes a `[T; CAP]`
  pool + nullable `Option<usize>` index handles (Part B). `free` of a
  recognized allocation is a no-op (the backing/pool drops at scope end;
  a defined program never reads freed storage). OUT (still rejected,
  located): realloc (the fixed backing cannot resize); a RETURNED heap
  pointer (a callee-local backing would dangle -- keeps binary-tree out);
  an unbounded/non-foldable pool capacity; a heap allocation that escapes
  (global store, address-of, non-`free` call). `Vec<T>` is the documented
  future generalization for an unfoldable capacity.

### Hosted library surface (beyond the language)

- [x] C99-47 The full printf format language: %s, %c, %u, %x, %o, %e,
  %g, %p, field width, precision, flags, and length modifiers, mapped
  onto Rust format specifications. CLOSED as supported-subset plus
  documented rejections: every form below is either byte-exact against
  glibc or carries a located rejection pinned by a negative test —
  silent divergence is structurally excluded. The supported grammar is
  `%[flags][width][.precision][length]conv` with all five C99 flags
  (`-`, `0`, `+`, ` `, `#`), decimal width and precision (a bare `.`
  is precision 0), lengths `l`/`ll` (i64/u64) and `h`/`hh` (the
  promoted argument reduced to short/char range by an `as`-cast, C99
  7.19.6.1p7 masking semantics), and conversions d/i, u, x/X, o, c, s,
  f/F/e/E/g/G, and %%.
  Directives that map 1:1 onto Rust format specs keep the direct
  mapping (`%5d`→`{:5}`, `%-5d`→`{:<5}`, `%05d`→`{:05}`,
  `%04X`→`{:04X}`; Rust's zero pad is sign-aware like C's; on %c/%s a
  width is explicit alignment, `{:>5}`/`{:<5}`, because C right-aligns
  text where Rust's string formatting left-aligns). Everything beyond
  that subset routes through on-demand module-level helpers
  implementing the C99 rendering rules exactly, validated byte-exactly
  against glibc printf on a structured battery plus fuzzed batteries of
  4000 random f64 bit patterns and 4000 random integers across the
  flags/width/precision matrix (zero diffs): `__emitrust_fmt_int` (+
  `__emitrust_fmt_i64`/`__emitrust_fmt_u64` wrappers) renders integer
  precision (digits zero-padded after the sign; value 0 with precision
  0 prints nothing; `0` ignored next to a precision or `-`), the
  `+`/` ` sign slots, and `#` alternate forms (octal leading zero only
  when needed, 0x/0X on nonzero values, prefixes inside the `0` width
  padding); `__emitrust_fmt_float` (+ `__emitrust_fmt_edigits`/
  `__emitrust_fmt_exp`) renders f/e/g on Rust's exact correctly-rounded
  decimal conversion (`{:.*}`/`{:.*e}` round half-to-even on the exact
  binary value, matching glibc): %e with sign-always two-digit
  exponents, %g with the C99 f/e style switch on the post-rounding
  exponent, trailing-zero trimming, and glibc's `%#g` rounding-carry
  quirk (a carry landing exactly on the 10^P decade drops the mantissa
  fraction — `%#g` of 999999.5 is `1.e+06` — while an exact power of
  ten keeps it, `1.000e+04`; detected via the shortest-round-trip
  pre-rounding exponent); non-finite values print C's spellings
  (`inf`/`nan`, uppercase under F/E/G, `-nan` when the sign bit is
  set) and pad with spaces even under `0`, as glibc does. Bare
  %f/%lf keeps the `__emitrust_fmt_f64` fast path. %s gained precision
  (`%.Ns`): a string literal truncates at import time (only the
  retained prefix is validated), the slice shapes route through
  `__emitrust_cstr_n`, which stops at N bytes or the first NUL,
  whichever comes first (C99 7.19.6.1p8: no terminator needed when the
  precision bounds the read). u/x/X/o still `as`-cast the argument to
  the directive's unsigned type (`%x` of -1 is ffffffff) and
  mismatched integer widths truncate like C's x86-64 varargs read.
  Rejected by policy, each with a located diagnostic and a pinned
  negative test: %p (pointer provenance is compiled away by the FR-28
  decomposition, so no address exists to print), %n (writes through a
  pointer), %a/%A (hex float), `*` width/precision (runtime-supplied),
  lengths `L` (no long double representation) and `j`/`z`/`t`,
  undefined-by-C99 flag combinations rejected rather than silently
  dropped (`%#d`, `%+u`, `%0c`, `%010s`, `%.3c`), wide %lc/%ls,
  h/hh/ll on floating conversions, widths/precisions over nine digits,
  NaN floating-point *constants* at the Rust-emission boundary (Rust
  does not guarantee its NAN constant's sign/payload; infinity
  constants are emitted as `f64::INFINITY`/`f64::NEG_INFINITY`), and
  argument type mismatches. The char-path helpers stay ASCII-only by
  design (C99-48).
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
  (test/Import/C/printf.c, printf-extended.c, printf-precision.c,
  printf-float-forms.c, printf-extended-invalid.c, sprintf-invalid.c,
  strings.c, strings-invalid.c, test/Import/C/printf-user-defined.c,
  test/EndToEnd/printf-formats.c — differential against the clang-built
  native binary over every supported form with boundary values —
  test/EndToEnd/strings.c, test/EndToEnd/printf-user-defined.c,
  test/EndToEnd/printf-user-defined-nonvariadic.c)
- [x] C99-48 A curated stdio/stdlib/string/math subset mapped to Rust
  equivalents (putchar, puts, abs, string functions over the C99-28
  representation, math intrinsics onto f64 methods), each function
  individually tested differentially. Curation complete: every function
  below is either mapped to safe Rust over the decomposed representation
  (no libc linkage, no unsafe) or rejected with a located diagnostic and
  a recorded rationale; every intercept carries the `!getDefinition()`
  guard (a project-supplied function of a curated name imports as an
  ordinary call), and every UNCURATED libc function keeps the
  system-header use-site rejection ("declared in a system header; not
  part of the supported C subset"), pinned by negative tests (rand,
  strtok, strstr, tgamma). stdio: statement-position
  `puts(s)` and `putchar(c)` are lowered by name when the project
  supplies no definition of its own (a user-defined puts/putchar stays
  an ordinary call): puts to `println!` through the %s machinery (both
  %s shapes), putchar to `print!` of the argument through
  `__emitrust_fmt_c`, matching C's conversion to unsigned char. The
  char helpers are ASCII-only by design: C writes the raw byte where
  Rust would encode code points 128..=255 as two UTF-8 bytes, so
  non-ASCII string data is rejected at import (see C99-28/47) and the
  helpers are exact for everything that gets through. Value uses of the
  puts/putchar result keep located rejections.
  math.h (doubles are f64): definition-less fabs/sqrt/floor/ceil with
  the standard double(double) prototype lower to the matching f64
  methods (`f64::abs`/`f64::sqrt`/`f64::floor`/`f64::ceil`) — these are
  IEEE-754-exact operations (fabs/floor/ceil exact, sqrt correctly
  rounded), so every conforming implementation agrees bit for bit and
  the mapping needs no libm argument at all. `sin` lowers to `f64::sin`
  on the weaker "both sides resolve to the platform libm" argument,
  verified differentially. exp/log/pow are REJECTED by policy with the
  located "has no bit-exact Rust mapping" diagnostic — the recorded
  rationale: C imposes no accuracy requirement on them (C99 F.9 makes
  no correctness guarantee), libm implementations disagree in the last
  bits, and rustc may constant-fold a constant argument through a
  different libm than the differential oracle's glibc, so no
  bit-exactness argument can be made; widening the pinned sin exception
  was considered and declined. Every other math function keeps the
  system-header rejection (the `hostedMathCallee` table in ImportC.cpp
  is the extension point).
  stdlib.h: definition-less abs/labs lower to
  `i32::wrapping_abs`/`i64::wrapping_abs` — abs(INT_MIN)/labs(LONG_MIN)
  is C UB (7.20.6.1p2), refined to the deterministic two's-complement
  wrap the oracle's platform also produces (Rust's plain `abs` panics
  only in debug profiles and was rejected as profile-dependent). atoi
  parses its argument's char region (the strlen shapes: a char array, a
  literal backing, or a pointer into either) through the one-per-module
  `__emitrust_atoi` helper with C's exact 7.20.1.2 semantics: skip
  isspace bytes, one optional sign, decimal digits to the first
  non-digit, 0 when no digits exist (leading junk included); values out
  of range are C UB (7.20.1p1) refined to deterministic i32 wrapping,
  and a region ending before any terminator stops at the region end
  (reading past the array is C UB, refined — every helper access stays
  a bounds-checked slice index). Statement-position `exit(status)`
  lowers to `std::process::exit(status as i32)`, matching C's
  termination and exit-status semantics (both report the low byte on
  this target, pinned by the exit-status differential); exit returns
  void in C, so no value position exists. Every other stdlib function
  (rand, strtol, qsort, general malloc/free beyond the CTS-P4
  single-constant-size calloc/malloc carve-out, ...) keeps the
  system-header rejection.
  string.h (CTS-L1): definition-less strcpy/strncpy/strcat/memset/
  memcpy/memmove lower by name in statement position, strcmp/strncmp/
  memcmp and strlen in value position, and strchr/strrchr where a printf
  %s argument or a null-pointer comparison consumes the result — each to
  a one-per-module safe Rust helper over `&[i8]`/`&mut [i8]` slices of
  the argument's char region (a char array, a string-literal backing, or
  a pointer into either), so every access is a bounds-checked slice
  index with no unsafe. Comparison helpers compare as unsigned char per
  C; strchr/strrchr return the found index or -1 (C's NULL), which %s
  offsets into the region and null comparisons test directly;
  same-object memcpy borrows the array mutably once and passes both
  cursors (`copy_within`, refining C's undefined overlap). memmove
  shares memcpy's lowering EXACTLY, and the mapping is exact rather than
  a refinement: distinct char regions are distinct array objects and can
  never overlap, and the same-object shape is `copy_within`, which IS
  memmove's overlap-correct copy — no temporary needed. Copy results
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
  strings-hosted-invalid.c, math.c, libc-subset.c,
  libc-subset-invalid.c, test/EndToEnd/strings.c, strings-hosted.c,
  math-sin.c, libc-subset.c)

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

Ledger as of 2026-07-20: 220 total / 220 passed / 0 miscompiled /
0 unsupported — COMPLETE (was 150/70 at commit a091423, when this checklist was
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
fwrite/fgetc/fgets/fclose, the C99-48 stdio slice [+00187]; the 00209
wave took 217 -> 218: K&R callsite-prototype inference, FR-29; the CTS
00204 wave took 218 -> 219: long-double-as-f64 (C99-8 revision) +
va_list monomorphization (C99-37 revision) + `const char **`
string-cursor parameters + keyword-function mangling + call-result
temporaries [+00204]; and the CTS-BR wave took 219 -> 220: the u8-only
byte-region aggregate model [+00216]). THE SUITE IS COMPLETE:
220/220 passed, 0 miscompiled, 0 unsupported — every former
permanent-out disposition was overturned by a dedicated spike-scoped
TDD wave (2026-07-19/20). The overturned reasonings are preserved
below inside each LANDED entry for the record.
Per-test dispositions:
- 00204 PASSES (disposition OVERTURNED 2026-07-19; formerly
  PERMANENT-OUT on "struct-typed varargs / HFA calling convention,
  fundamentally outside safe-Rust emission"). What changed: the HFA
  calling convention never needed modeling — per-call-site
  monomorphization (C99-37 revision) turns each `va_arg(ap, struct
  hfa34)` into a dispatch over ordinary by-value Copy parameters, and
  the long-double blocker dissolved into the f64 substitution (C99-8
  revision), sound here because every 00204 value is f64-exact at one
  printed decimal. The remaining companions (the keyword-named `match`
  helper with its advancing `const char **` cursor, `fr_hfa12().a`
  call-result member reads, `struct s1 t1 = fr_s1()` initializers)
  landed alongside. Byte-exact against the native oracle: 35 myprintf
  call sites, 33 distinct clone signatures, 14 struct-typed va_arg
  sites, %.1Lf output.
- 00209 LANDED (2026-07-19, overturning the earlier upheld rejection):
  K&R callsite-prototype inference (FR-29). Rule: a call with
  arguments whose callee, after the `(*fp)` deref-peel, is a
  `DeclRefExpr` to a local-storage ParmVarDecl/VarDecl of
  pointer-to-`FunctionNoProtoType` infers that decl's prototype from
  the call's argument types — clang has already applied the default
  argument promotions at a no-proto call (C11 6.5.2.2p6), so the
  promoted types are used verbatim (char -> i32 via `arith.extsi`,
  float -> f64 via `arith.extf`, the casts the ordinary typed-call
  conversion emits) — plus the declared return type. The decl then
  DECLARES at the refined `!emitrust.fn_ptr<promoted... -> ret>`
  (parameter and local place alike), its argument-carrying calls lower
  through the ordinary typed `emitrust.call_indirect` path, and
  binding a real function to it resolves against the refined
  signature. Multiple call sites for one decl must agree. Located
  rejections: a second disagreeing site is the NEW
  "unsupported: conflicting inferred prototypes for function pointer
  '<name>'" at that site; non-decl-traceable callees (struct members,
  array elements, call results) RETAIN "unsupported: call with
  arguments through a function pointer without a prototype"; an
  incompatible function bound to an inferred decl keeps
  "unsupported: function '<name>' does not match the function pointer
  signature" (resolveFunctionPointerDecl exact equality); and an
  unrefined no-proto VALUE passed into a refined position keeps
  "unsupported: call argument type mismatch" at the passing call site.
  A no-proto pointer never called with arguments stays at the
  unrefined `fn_ptr<() -> T>` mapping — inference is per-decl, not
  per-typedef (00209's f5 `fptr1` argument is untouched by f1's
  refinement of the same spelling).
  (test/Import/C/fnptr-noproto-infer.c, fnptr-noproto-infer-invalid.c,
  fn-pointers-invalid.c noproto-args; differential
  test/EndToEnd/fnptr-noproto-infer.c with executed inferred calls;
  c-testsuite 00209 — ledger 217 -> 218 passed / 0 miscompiled)
- 00216 PERMANENT-OUT (wave in flight): beyond its first blocker (flexible array
  member, "00216.c:46:12: error: unsupported: flexible array member" —
  the C99-17 dedicated wording that replaced the generic
  "non-constant array size" fallback) it
  requires byte-exact struct layout INCLUDING padding (a print macro
  walks `(u8*)&x` over sizeof(x)), GCC range designators, and
  compound literals with relocations — byte-exact ABI layout is
  antithetical to the project's safe-Rust value model (the same reason
  bit-field layout is deliberately non-ABI, see C99-45).
- 00216 LANDED (CTS-BR, the u8-only byte-region aggregate model): the
  former PERMANENT-OUT reasoning ("byte-exact layout is antithetical
  to the safe-Rust value model") only holds for aggregates with
  padding or mixed-width leaves. An aggregate whose scalar leaves are
  ALL `unsigned char` — u8 members, u8 arrays, nested such structs,
  u8-only unions including unnamed arms, empty structs contributing
  zero bytes, GNU zero-length arrays contributing zero, and a flexible
  array member contributing zero to sizeof — is padding-free BY
  CONSTRUCTION, so its object representation is exactly its value
  representation and safe Rust can model it byte-exactly.
  Classification: such a record is a BYTE REGION; a union arm with
  non-u8 leaves is tolerated type-level only as a constant array whose
  size equals the union's (the in6_addr u16[8]-over-u8[16] alias);
  mixed-size non-u8 arms keep the "unsupported: union ..." rejection;
  any non-u8 leaf keeps the aggregate on the typed struct_def path.
  Representation: byte-region objects are plain `!emitrust.array
  <Nxui8>` (N == sizeof; arrays of byte-region records flatten to one
  n*sizeof region); global initializers fold to complete zero-filled
  byte images from the APValue against the target layout, with a
  static FAM-tail initializer folding into an EXTENDED image while
  sizeof stays FAM-free (gw: 22-byte sizeof, 30-byte image); locals
  are bare zero-defaulted `emitrust.variable` regions initialized per
  byte (folded ui8 constants, embedded per-byte region copies for
  struct-value elements, runtime scalars through their AST casts);
  member access is `emitrust.subscript` at the member's constant byte
  offset; `(u8 *)&x` is the region base (a byte view of a NON-u8
  aggregate rejects at the cast: "unsupported: byte view of an
  aggregate with non-byte members"); `&x.member` is base + offset;
  struct copy/assign/init-from-deref are per-byte region copies.
  Pointers to byte-region records are `!emitrust.slice<ui8>`
  parameters (shared `&[u8]` for const pointees) riding the slice
  decomposition with byte-granular cursors; byte-region GLOBALS passed
  by address stage a whole-image copy, with mutable parameters storing
  the image back after the call. FAM/zero-length RUNTIME accesses
  reject per the amended C99-17. The remaining 00216 constructs also
  landed: GCC range designators arrive pre-expanded in clang's
  semantic form; fn-ptr TABLES (`T (*t[N])(...)`) import as
  `array<Nx!emitrust.fn_ptr<...>>` globals with folded Some(target)
  elements, `t[i]()` is subscript + call_indirect, and a runtime store
  to a slot rejects ("unsupported: assignment to a function-pointer
  array element"); a `void *` struct member whose every stored value
  is the address of one signature's function (the T1.1 holder bound
  extended to members) retypes to a fn_ptr member, readable under a
  cast to that signature. Byte-exact layout INCLUDING padding remains
  out for non-u8 aggregates (the same reason bit-field layout is
  deliberately non-ABI, see C99-45).
  (test/Import/C/byte-region-aggregates.c, byte-region-init.c,
  byte-region-aggregates-invalid.c, fnptr-table.c,
  fnptr-table-invalid.c, flexible-array-invalid.c,
  test/EndToEnd/byte-region-walk.c differential; ledger +00216.)
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
  (see its entry). Three of the four listed tests passed here first;
  the fourth, 00209, cleared its pointer-global blocker here and later
  flipped to PASS when the K&R callsite-prototype inference wave
  (FR-29, 2026-07-19) landed — see the 00209 disposition above.
  (00040.c, 00045.c, 00149.c, 00209.c — all PASS)
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
  0 miscompiled. 00209 left CTS-P4 scope here: after the
  pointer-to-fn-ptr parameter and fn_ptr slice extensions landed
  (pointers-fnptr-slice.c), its sole remaining blocker was the
  argument-carrying K&R `int (*)()` call in f1 (C99-46 scope, not
  CTS-P4), at the time upheld as a by-design rejection and since
  overturned by the FR-29 callsite-prototype inference wave
  (2026-07-19) — 00209 now passes; see its disposition above.
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
- [x] Array-member self-reference (FR-37/FR-38, `union-find.c`'s
  `struct uf_node *parent` shape): `planArrayMemberPointers`
  (`lib/ImportC/ImportCPlanning.cpp`) proves a struct pointer MEMBER
  whose every read/write site provably roots in the SAME promoted
  owner-struct array as the struct instance it lives in (never an
  externally-named distinct object) and synthesizes an
  `emitrust.enum_def` with one variant per array INDEX — not per
  distinct base object — decoded/written via a genuine `match` on that
  closed, array-sized set. Key architectural decision: this reuses and
  extends the owner-struct-index model (`planOwners`, FR-30/FR-36)
  rather than generalizing CTS-P7's enum-of-BASES multi-base cursor to
  cover struct members. CTS-P7's mechanism is the right fit for a small,
  statically enumerable set of genuinely DISTINCT named objects a single
  local pointer variable ranges over (`p = &x; ... p = &y;`) — each
  variant names one independent base. A self-referential array-member
  pointer is a different shape: it ranges over elements of ONE already-
  promoted array (potentially large, and sized by the array, not by a
  fixed count of distinct locals), and it must interoperate with that
  same array's pointer PARAMETERS, LOCALS bound from calls (FR-36's
  owner-index return), and cross-parameter equality (FR-38's B5) — all
  of which already speak the owner-struct i64-cursor representation.
  Building the member pointer on owner-struct-index instead of a second,
  CTS-P7-shaped discriminated union means every one of those
  interactions (path compression's `parent = x->self;` field read
  binding a local, `while (x->self != x)` comparing a decoded field
  against a cursor, `root1 == root2` comparing two independently-traced
  parameters) is the SAME representation on both sides with no bridging
  conversion between two different pointer models. CTS-P7 stays scoped
  to its original multi-object-local use case and is untouched by this
  work (`pointers-multi-base.c` and its differential counterpart pass
  unchanged). See FR-37/FR-38 above for the mechanism's staged
  construction (enum synthesis, path compression, cross-parameter
  equality) and `test/EndToEnd/union-find.c` for the full capstone
  program this unlocks.
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
  ANONYMOUS union member — mixed-type arms, an arm wider than one
  slot — keeps the located union-type rejection; named and bare union
  TYPES were later admitted by the C99-44/CTS-R3 one-slot model, whose
  wider pun matrix does NOT extend to anonymous members (identical
  leaves only here).
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
- [x] CTS-R3 (3) Unions (C99-44): CLOSED — all three target tests PASS
  in the manifest, and the design decision is the C99-44 one-slot
  struct model (a supported subset with documented located rejections;
  neither of the original candidates — data-carrying enum, byte-array
  storage with accessor helpers — was taken).
  (00042.c, 00210.c, 00218.c — all PASS)
  Wave5 B1: the one-slot struct model (full matrix under C99-44)
  admits unions whose arms alias one leaf — identical mapped types,
  same-width integers differing only in signedness (bit-exact
  `emitrust.cast` reinterpretation at the accesses), and, since the
  float-pun extension, a float arm against a same-width integer
  (bit-exact `emitrust.bitcast`, Rust to_bits/from_bits) — flipping
  00042.c (untagged local two-int-arm union) to PASS.
  T1.1: the byte-array arm (00210's `uint16_t u; uint8_t b[2];`,
  packed attributes in either typedef position tolerated and discarded)
  ADMITS at the TYPE level: the slot is the INTEGER arm regardless of
  declaration order, the array spelling never reaches the IR, and any
  access through the array arm is a located
  `unsupported: union byte-array arm access` at the ACCESS site;
  unequal-total-width array arms keep the union family rejection at the
  union decl. Together with the local void* fn-ptr holder (a
  never-reassigned local `void *` initialized from one known
  non-variadic function whose every value use is an explicit cast to
  exactly the target's signature in callee position imports as an
  ordinary `!emitrust.fn_ptr` local — fn-address `Some(target)`
  constant + `emitrust.call_indirect`, the cast fully peeled;
  out-of-shape holders keep `unsupported: pointer assigned a
  non-address value`), 00210.c flipped to PASS.
  00218.c (a SINGLE-ARM union of a struct with pointer members and an
  enum bit-field — not a multi-arm pun) passes through the one-slot
  single-arm admission combined with the C99-45 enum-bit-field
  zero-extend accessors and the CTS-P2 pointer-struct-member work; the
  earlier note here calling it out of scope was stale.
  Union shapes outside the model stay behind located
  `unsupported: union ...` rejections (test/Import/C/unions-invalid.c,
  union-bytearray-arm-invalid.c); positive pins in
  test/Import/C/unions.c, union-bytearray-arm.c, fnptr-void-local.c
  (+ -invalid), test/EndToEnd/unions.c, fnptr-void-local.c.
  Ledger at this entry's closure (T1.3 era): 217 passed /
  3 unsupported / 0 miscompiled of 220; superseded — see the ledger
  header above (218 as of 2026-07-19).
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
  rename is complete for it too — and, since the 2026-07-19 CTS 00204
  wave (long-double-as-f64 + va_list monomorphization; see the
  disposition list above), passes end-to-end as well.
  (00129.c, 00204.c, 00219.c)
- [x] CTS-R6 (1) Empty structs (`struct T {};` — a GNU/C2x shape clang
  accepts): emit a unit-like Rust struct.
  Empty-struct support landed: the importer accepts a field-less
  record, struct_def permits empty field arrays, and the emitter prints
  `struct T {}` (declaration/copy/default via the usual derives;
  test/Import/C/structs-empty.c, Dialect ops.mlir, Target memory.mlir).
  The eager file-scope emission defers for an empty struct no
  declaration type mentions (CTS-BR: one that only ever appears as a
  zero-byte member of a byte-region aggregate never emits a
  struct_def). 00216.c itself landed via the CTS-BR byte-region wave —
  the FAM declaration is tolerated per the amended C99-17 and the
  whole test is on the manifest.
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
Not itemized above: printf precision (`%.3s`) and the ll/h/hh length
specifiers are now inside the C99-47 grammar; the L length modifier on
the floating conversions joined it with the CTS 00204
long-double-as-f64 policy (C99-8 revision — bare %Lf keeps the
__emitrust_fmt_f64 fast path, adjusted forms route through
__emitrust_fmt_float), while `%Ld` (L on an integer conversion) stays
rejected. No test is sole-blocked on printf forms today (00182.c and,
since the 00204 wave, 00204.c pass).

## C++ input (subset)

- [x] W2.0 Per-input C++ frontend selection, `namespace`/`extern "C"`
  AST tolerance, and a `class`-as-`struct` data-only import, opening a
  narrow C++ INPUT subset without touching member functions (deferred
  to a later, spike-gated wave). Motivation: `buildCommandLine` hardcoded
  `-std=c11`, so the clang driver rejected every `.cpp` input outright
  before any AST visiting could happen; the translation-unit decl loop
  dispatched only `FunctionDecl`/`RecordDecl`/`EnumDecl`/`TypedefDecl`/
  `VarDecl` and silently skipped anything else, so `NamespaceDecl` and
  `LinkageSpecDecl` (`extern "C"`) members never got visited at all; and
  `collectRecordFields` walked `fields()` only, silently dropping any
  base class's data — a real data-loss hazard once `CXXRecordDecl`
  reached `importRecord`.
  Landed: (1) **Frontend selection** — `buildCommandLine` takes an
  `isCxx` flag and emits `-x c++ -std=c++17` (dropping `-std=c11`
  entirely, since clang hard-errors on the two together) for a source
  whose extension marks it C++ (`.cpp`/`.cc`/`.cxx`/`.C`/`.c++`/`.hpp`,
  `isCxxSourcePath`); a new `PerFileCompilationDatabase` picks the C or
  C++ `FixedCompilationDatabase` per input file, so `importCProject`'s
  multi-file `ClangTool` can compile each input in its own language
  (mixing them into one linked program stays out of scope). (2) **TU-loop
  tolerance** — the per-decl dispatch that used to be inline in
  `importTranslationUnit`'s loop is factored into `importDeclsIn`, which
  recurses into a nested `NamespaceDecl` or `LinkageSpecDecl` exactly as
  if its members were declared at the enclosing level (both are
  `DeclContext`s, so the same function recurses on either); a plain C
  program never contains either decl kind, so its behavior is
  unchanged. Namespace membership flattens into the emitted symbol
  name (`namespacePrefix`: `ns_<name>_` per level, outer-to-inner,
  composing for nested namespaces; `ns_anon_` for an anonymous
  namespace); `extern "C"` contributes no prefix (unchanged C linkage
  name). `mlirFuncName` and the new `globalVarSymbolName` (factored out
  of `importGlobalVar` so `collectOrdinaryNames`'s pre-scan — itself
  given the matching recursive walk, `collectOrdinaryNamesFrom` — always
  agrees) both apply the prefix; call sites and variable reads already
  resolved symbols by decl identity (a `DenseMap`/`mlirFuncName`
  recomputation), not by the call's source spelling, so a
  namespace-qualified call (`shapes::detail::helper(4)`) needed no
  separate handling. (3) **`class`-as-`struct`** — `importRecord`
  accepts `isClass()` alongside `isStruct()`/`isUnion()` (the keyword
  only changes the default member access, which the importer already
  ignores: it walks `fields()`, which never yields an `AccessSpecDecl`
  or a `CXXMethodDecl`). `collectRecordFields` rejects a `CXXRecordDecl`
  with any base class with a located `"unsupported: base classes are not
  supported"` instead of silently dropping the inherited data. Methods
  (virtual or not) on a class with no base classes are silently absent
  from the import rather than rejected — they are never visited at all,
  since a `CXXRecordDecl`'s methods live in the record's own
  `DeclContext`, never as siblings of the record in the enclosing scope
  the TU-loop walk visits. KNOWN GAP, explicitly accepted this wave: a
  class with virtual methods and no base classes still imports as a
  plain field-only struct, with no vtable-pointer slot — silently wrong
  against the real Itanium C++ ABI layout, but harmless for THIS
  transpiler's own semantics (it never executes a virtual call or
  compares `sizeof` against a foreign compiler's layout; the whole
  program is reinterpreted through its own struct definition
  consistently everywhere). Full method import (including rejecting or
  supporting virtual dispatch) is W2.2's job. (4) **References** reject
  with a dedicated `"unsupported: reference types are not yet
  supported"` in `mapType` (checked as its own case, ahead of the
  generic tail rejection a `T&`/`T&&` would otherwise fall into) —
  `mapParamType` already delegates a non-pointer parameter type to
  `mapType`, so a reference parameter picks this up with no separate
  change. (5) **Two AST-tolerance fixes the positive test itself
  surfaced**, both outside the original survey: C++'s `true`/`false`
  keywords produce a `CXXBoolLiteralExpr` (unlike C's `stdbool.h`
  macros, which expand to a plain `IntegerLiteral`); `emitRValue` gained
  a case folding it to the same i1 constant a `_Bool` literal would. And
  a class/struct-typed local or global declared with NO explicit
  initializer carries an implicit `CXXConstructExpr` calling the
  (possibly trivial) default constructor in C++, where C has no
  initializer expression at all for the same declaration; a new
  `significantInit(var)` helper strips this wrapper back to "no
  initializer" ONLY when the constructor is trivial
  (`CXXRecordDecl::hasTrivialDefaultConstructor()`) and takes no
  arguments — a class with a genuinely non-trivial default constructor
  (user-provided, or a non-trivial member) still carries a real
  `CXXConstructExpr` after this check and correctly falls through to the
  existing generic aggregate-initializer rejection, since silently
  skipping real constructor side effects would be a miscompile, not a
  merely unsupported construct. Every other C++-only construct
  (templates, virtual dispatch, multiple/virtual inheritance, operator
  overloading, exceptions, `if`/`switch` init-statements, lambdas, ...)
  is untouched and falls through to whatever generic rejection already
  existed (`"unsupported top-level declaration"` for a
  `FunctionTemplateDecl`, `"unsupported statement: CXXTryStmt"` for
  `try`/`catch`); pinning those baseline wordings is deliberate so a
  later wave that DOES implement one of them has a documented starting
  point instead of discovering the message fresh.
  Gates: warning-free build; `check-emitrust` 290/290 (287 pre-existing +
  3 new: `test/Import/Cpp/cpp-basics.cpp`, `cpp-basics-invalid.cpp`,
  `test/EndToEnd/cpp-basics.cpp`, the last using `clang++` as the native
  differential leg — `test/lit.cfg.py` gained the `.cpp` suffix so lit
  discovers them); c-testsuite ledger unchanged at exactly 220/220/0/0;
  a byte-identical `--emit=rust` snapshot over every pre-existing
  `test/EndToEnd/*.c` file (the C path is untouched by construction: none
  of the new code paths — `PerFileCompilationDatabase`'s C branch is the
  same flags as before, `importDeclsIn`'s recursion is dead code for any
  decl kind C can produce, `namespacePrefix` is empty whenever no
  `NamespaceDecl` exists, `significantInit` only ever strips a
  `CXXConstructExpr` C never produces — can change a C program's
  import); a 150-seed differential fuzz campaign (seeds 9850-9999,
  `--range-check`), 150/150 passed, 0 miscompiled, 0 range violations,
  oracle agreement 150/150 (the fuzz generator has no C++ templates, so
  this campaign is a pure C-path regression check, not new C++ coverage).
  OUT of scope for this wave, staying rejected or silently absent
  (tracked for later waves): member functions/constructors/destructors
  (W2.2, spike-gated), base classes, references, templates, virtual
  dispatch and multiple/virtual inheritance, operator overloading,
  exceptions, lambdas, `if`/`switch` init-statements, and mixing C and
  C++ inputs into one linked program.
  (test/Import/Cpp/cpp-basics.cpp, cpp-basics-invalid.cpp;
  test/EndToEnd/cpp-basics.cpp)
- [x] W2.2 Non-virtual C++ class methods, constructors (member-initializer
  lists), overloads, and `static`/`const` receivers land on the existing
  `emitrust.impl`/`emitrust.method_of`/`emitrust.method_call` surface
  (proved additive-only by a throwaway risk-gate spike,
  spike-methodcall). Landed: (1) **`emitrust.static_method`** — a new
  discardable unit attribute (`include/EmitRust/EmitRustDialect.h`)
  marking a `method_of`-tagged function that takes no receiver;
  `emitrust.impl`'s verifier (`EmitRustOps.cpp`) skips its receiver-shape
  check entirely for such a function, and additionally now accepts an
  `!emitrust.ref<struct>` (not just `!emitrust.mut_ref<struct>`) receiver
  on an ordinary method — the const (`&self`) receiver shape. (2)
  **`convert-func-to-emitrust`** (`FuncToEmitRust.cpp`) accepts a
  method-call receiver borrow produced by `emitrust.addr_of` of EITHER
  mutability (previously mutable-only): a call to a const method borrows
  shared. (3) **The Rust emitter** (`TranslateToRust.cpp`) renders a
  static method's parameter list with no `self` at all, and an ordinary
  method's receiver as `&self` or `&mut self` depending on whether its
  entry-block argument 0 is `!emitrust.ref` or `!emitrust.mut_ref`.
  (4) **The per-class mangling scheme** (binding; the spike had hard-coded
  the literal name `"new"` for its one class and inherited
  `mlirFuncName`'s C-storage-class tag, which happened to give C++
  `static` members the SAME per-TU tag as a C file-static function for
  unrelated reasons — decoupled here):
  `<StructName>_<methodBaseName>[_<overloadSuffix>]`, computed once by
  `CImporter::cxxMethodMangledName` (`ImportCFunctions.cpp`) and used
  identically for the imported `func.func` symbol, every
  `method_call`/`call_opaque` reference to it, and constructor lookup.
  `<StructName>` is the class's already-assigned emitrust struct name;
  `<methodBaseName>` is `"new"` for a constructor (whose
  `DeclarationName` has no ordinary identifier — `CXXConstructorName` is
  a distinct `DeclarationName::NameKind`) or the method's C++ name
  passed through the existing `mangleMemberName` keyword-escape, exactly
  like a struct field; `<overloadSuffix>` appears only when the class
  declares more than one method (or constructor) sharing the same base
  name, as the declaration-order concatenation of each parameter's
  overload type code (`i` for `int`, `b` for `bool` — the two builtin
  types this wave's fixtures overload on; the table is deliberately
  small and extends as later waves need more codes) — a zero-parameter
  member of an overload set contributes an empty code string and so
  keeps the bare, unsuffixed spelling. A static call site
  (`Counter::origin()`) has no receiver to borrow and so is never an
  `emitrust.method_call`; it lowers to a direct `emitrust.call_opaque`
  naming the fully qualified `"<StructName>::<mangled-symbol>"`, reusing
  the already-mangled symbol on both sides of the `::` so the qualified
  string is always self-consistent with whatever the Rust emitter prints
  inside that `impl` block. (5) **Constructor lowering**: a constructor
  imports as a void `&mut self` method named via the scheme above (base
  name `"new"`); its member-initializer list (`CXXCtorInitializer`s,
  which live OUTSIDE `getBody()`, via `CXXConstructorDecl::inits()`) is
  lowered FIRST, in declaration order, ahead of the constructor's own
  compound-statement body — each initializer becomes `deref(self)` /
  `member(field)` / `assign`. A member-initializer that reads one of the
  constructor's own parameters binds the RAW entry-block argument
  directly rather than the memref cell the ordinary parameter-binding
  loop spills it into for the (unrelated) body, since the two lowerings
  are independent and the pinned IR shape names the untouched block
  argument. A local/global declared with a non-vacuous constructor call
  (`Counter c(5);`, or `Counter c2;` calling a non-trivial user default
  constructor — `significantInit`, W2.0) default-constructs its place
  (`emitrust.variable`) and then invokes the resolved constructor as an
  ordinary mutating method call on `&mut place`, discarding the (void)
  result. Copy/move/delegating constructors are out of scope and
  rejected (no value/aliasing semantics modeled for them yet). (6) **The
  importer walk** (`ImportCAggregates.cpp`): right after a class's
  `struct_def` is emitted, `importCXXMethods` walks every
  non-implicit, non-deleted `CXXMethodDecl` (mutating, const, static,
  and non-delegating/non-copy/non-move constructors) onto the impl
  surface via the existing `importFunction`, whose C++ path is entirely
  decoupled from `mlirFuncName` (no per-TU static-storage-class tag, no
  reserved-name/keyword-collision checks meant for C symbols). (7)
  **`this`** resolves directly to the method's own (undereferenced)
  receiver argument (`emitRValue`'s new `CXXThisExpr` case, backed by a
  new `currentCxxThisRef` field); `isDecomposedPointerExpr` short-circuits
  false for it so the existing `->`-base rvalue path in
  `emitMemberBasePlace` derefs it exactly like any other pointer-typed
  base — both the implicit (`value = ...`) and explicit (`this->value`)
  member-access spellings resolve to the identical place shape with no
  separate handling. `CXXMemberCallExpr` (`obj.method(...)`/
  `obj->method(...)`) lowers to `emitrust.method_call` via a new
  `emitCXXMemberCall`, mirroring the Phase-4 `emitMethodCallSite` shape
  but for a genuine object expression: the receiver borrow's mutability
  (`emitrust.addr_of` with or without `mut`) follows the resolved
  method's own const-ness. (8) **Located rejections** — three NEW this
  wave (a user-declared destructor, a virtual method, an overloaded
  operator), checked in `collectRecordFields` BEFORE any field (or
  method) of the class imports, so a rejected class never half-imports;
  four reused unchanged from W2.0 (base classes, references,
  `try`/`catch`, a class template) verified in a class-method context.
  KNOWN GAP surfaced by this wave and fixed as part of it, not merely
  documented: the CTS-BR byte-region classification (`isByteRegionRecord`)
  walks only a record's OWN `.fields()`, so a C++ class with zero data
  fields but at least one user-declared method (or any base class, or
  any virtual method — a vtable pointer) vacuously passed the
  all-fields-are-`u8` walk and was misclassified as a raw byte region;
  a pointer parameter or value declaration of such a class's type would
  then skip `importRecord` entirely, silently skipping BOTH its method
  import and its destructor/virtual/operator rejections. Never
  byte-region-classify a `CXXRecordDecl` with any base class, any
  virtual method, or any non-implicit method; a plain data-only
  class/struct (only compiler-synthesized special members) is
  unaffected. KNOWN GAP intentionally left OUT this wave: const/non-const
  overloads of the identical parameter signature (`void f(); void f()
  const;`) are not distinguished by the mangling scheme (only the
  parameter-type codes feed the overload suffix, never the method's own
  const-qualification) — a class declaring both would collide on one
  symbol; none of this wave's fixtures do, and disambiguating this is
  deferred to a later wave.
  Gates: warning-free build; `check-emitrust` green with zero
  regressions (the 4 new/flipped files —
  test/Import/Cpp/methods.cpp, methods-invalid.cpp,
  test/EndToEnd/cpp-methods.cpp, cpp-methods-overload.cpp — the last two
  byte-identical against `clang++ -std=c++17`); c-testsuite ledger
  unchanged at exactly 220/220/0/0; a byte-identical `--emit=rust`
  snapshot over every pre-existing `test/EndToEnd/*.c` file (captured
  before and after this wave's `lib/` changes — the C path shares
  `emitFunc`, `ImplOp::verify`, and `CallOpConversion::rewriteMethodCall`
  with the new C++ method surface, so this is not a vacuous check); a
  150-seed differential fuzz campaign (seeds 10000-10149,
  `--range-check`), 150/150 passed, 0 miscompiled, 0 range violations,
  100% oracle agreement (the fuzz generator has no C++ templates, so
  this campaign is a pure C-path regression check, not new C++
  coverage).
  OUT of scope for this wave, staying rejected or silently absent
  (tracked for later waves): inheritance, virtual dispatch, templates,
  exceptions, user-declared destructors, operator overloading,
  references (as parameters or as data members), copy/move/delegating
  constructors, and the const/non-const-overload mangling collision
  noted above.
  (test/Import/Cpp/methods.cpp, methods-invalid.cpp;
  test/EndToEnd/cpp-methods.cpp, cpp-methods-overload.cpp)
- [x] W2.3 STL recognition: `std::vector<T>` and `std::string` USAGE map to
  `Vec<T>`/`String` via the existing `!emitrust.opaque` +
  hosted-library-recognition precedent (the printf/FILE* dispatch,
  `ImportCHosted.cpp`) — WITHOUT ever importing a single libstdc++
  internal field. Motivation: `mapType`'s generic `RecordType` path
  called `importRecord` on ANY record, which for a USED
  `std::vector<int>` specialization would recurse into libstdc++'s real
  (private, allocator-bearing, pointer-heavy) layout and reject deep
  inside a system header with an unhelpful diagnostic; the fix diverts
  BEFORE that recursion.

  **Type recognition** (`CImporter::mapStdLibraryType`,
  `ImportCTypes.cpp`): `mapType`'s `RecordType` branch checks
  `decl->isInStdNamespace()` (which transparently unwraps libstdc++'s
  `std::__cxx11` inline namespace) BEFORE calling `importRecord`; a
  plain C program never has a `NamespaceDecl` in any `DeclContext`
  chain, so this check is unconditionally false there — dead code for
  the C path. A `ClassTemplateSpecializationDecl` named `vector` maps to
  `!emitrust.opaque<"Vec<<T>>">`, where `<T>` is `T` mapped RECURSIVELY
  through `mapType` (so an unsupported element type — a raw pointer, an
  unrecognized nested `std::` entity — is rejected with mapType's OWN
  located diagnostic, not re-worded) and re-spelled by
  `rustSpellingForElementType`: signed/unsigned integers of width
  8/16/32/64, `i1`->`"bool"`, `f32`/`f64`, an `!emitrust.struct` by its
  bare name, or another recognized STL opaque by its own spelling
  verbatim (nested containers — `Vec<Vec<i32>>`, `Vec<String>` — compose
  for free through the same recursion, though untested/unexercised this
  wave). `parseStlElementType` is the exact inverse, used at
  `operator[]`/`at()` sites to reconstruct a genuinely typed element
  place from the opaque's inner spelling. `basic_string<char, ...>`
  (i.e. `std::string`, since the alias resolves to its canonical
  specialization before `mapType` ever sees it) maps to
  `!emitrust.opaque<"String">`; a `basic_string` over any other
  character type (`std::wstring`, ...) is a located rejection. Every
  OTHER `std::` entity used as a value type (`std::map`, `std::set`,
  ...) is a located rejection naming it:
  `"unsupported: std::<name> is not a recognized STL type"`. A `std::`
  entity that is never actually USED (an unused `<vector>`/`<string>`
  declaration) never reaches `mapType` at all — the existing
  system-header SKIP already excludes it, unchanged by this wave. A
  reference to a NEVER-IMPORTED system-header global (`std::cout`) hits
  the PRE-EXISTING system-header-reference rejection before `mapType`
  runs at all, so `std::cout << x;` needs no dedicated STL-side handling.

  **Dialect widening** (`EmitRustOps.td`/`EmitRustOps.cpp`, two
  verifiers loosened, both C++-only-triggered): `emitrust.method_call`'s
  receiver may now be an lvalue of `!emitrust.struct` OR
  `!emitrust.opaque` (the STL method-call surface below — no other
  change to the op, which was already receiver-mutability-agnostic);
  `emitrust.subscript`'s array operand may now be an lvalue of
  `!emitrust.array`/`!emitrust.slice` (structurally cross-checked, as
  before) OR `!emitrust.opaque` (the `Vec<T>` `operator[]`/`at()`
  surface — TRUSTED rather than cross-checked, since an opaque string
  does not structurally decompose into an element type; this mirrors
  `emitrust.call_opaque`'s existing trust model for opaque interop).
  `RustEmitter::emitDefaultValue` (`TranslateToRust.cpp`) gained a
  `Vec::new()` default for a `Vec<...>`-spelled opaque, alongside the
  pre-existing `String::new()`/`__EmitrustFile::Null` cases — required
  because `emitrust.variable`'s renderer emits this default
  UNCONDITIONALLY for a no-initializer place, even though
  `emitStlConstruct` (below) always follows up with an explicit
  `emitrust.assign` immediately after; the default is therefore dead
  code for every program this wave's importer produces, but still
  required for the renderer to accept the IR at all.

  **Construction** (`CImporter::emitStlConstruct`,
  `ImportCStatements.cpp`; dispatched from `emitLocalVar`'s new
  `isStlOpaque` branch, parallel to the aggregate branch — an STL
  opaque local lives in an `emitrust.variable` place exactly like a
  struct local, since a memref of a dialect type is illegal, same as
  the enum/fn_ptr reason). Every C++ class-typed declaration with no
  explicit initializer still carries a synthesized `CXXConstructExpr`
  (`significantInit`, W2.0); UNLIKE a POD struct's vacuous default
  constructor, neither `std::vector`'s nor `std::string`'s default
  constructor is TRIVIAL (both have real, observable
  empty-container-producing behavior), so `significantInit` NEVER
  strips it — every STL local's construction is significant. Recognized
  shapes: a zero-argument default construction -> `Vec::new()` /
  `String::new()` (via `emitrust.call_opaque`, mirroring the
  `__EmitrustFile`/sprintf-staging-String precedent exactly); for
  `String` only, a single-argument construction from an ordinary string
  literal (`std::string s = "literal";`) -> `String::from("literal")`,
  the literal escaped by a NEW shared helper, `emitRustStrLiteral`
  (factored out of `emitPrintfStringArg`'s existing `%s`-literal
  escaping with ZERO behavior change — verified by a `check-emitrust`
  regression on `Import/C/strings-invalid.c`, which pinned the exact
  historical wording "... in printf '%s' string literal"; the factored
  function takes an explicit `context` string so each caller keeps its
  own wording). libstdc++'s converting constructor ALSO declares a
  defaulted allocator parameter, which `CXXConstructExpr` always fills
  with a `CXXDefaultArgExpr` even for a one-argument call site, so the
  argument-count check is "argument 0 is the literal AND every
  remaining argument is a `CXXDefaultArgExpr`", not a bare arity check.
  Copy/move construction is a located rejection (its own wording, sharper
  than the generic constructor-shape one). Every other constructor shape
  — the sized/fill vector constructor `std::vector<T>(n)`, an
  initializer-list constructor, `std::string` from a `char*` variable —
  is a located rejection: none of them are expressible in this wave's op
  vocabulary (`emitrust.call_opaque`'s plain `callee(args)` shape and
  `emitrust.method_call`'s `place.method(args)` shape cannot spell the
  `vec![v; n]` macro form a sized/fill constructor would need).

  **Method table**, PINNED (`CImporter::emitStlMemberCall`/
  `emitStlOperatorCall`/`emitStlVectorIndexPlace`, `ImportCExpressions.cpp`
  — intercepted at the top of `emitCXXMemberCall`
  and inside `emitCall`'s `CXXOperatorCallExpr` case respectively,
  before the generic imported-method lookup, which would never find a
  libstdc++ method; dispatch keys on `method->getParent()->
  isInStdNamespace()`, so a method on a user class is completely
  unaffected):

    | C++ | Rust | Notes |
    |---|---|---|
    | `v.push_back(x)` | `v.push(x)` | `emitrust.method_call`, void result |
    | `v.size()` | `v.len()` then cast | `.len()` returns `index`/`usize`; cast to the CALL EXPRESSION's own mapped type (`size_t`->`ui64`), not the assignment destination's type — a separate, ordinary conversion narrows further if needed |
    | `v[i]` (`operator[]`) | `v[i as usize]` | `emitrust.subscript` (a PLACE, not a value — see below) |
    | `v.at(i)` | `v[i as usize]` | renegotiated to the IDENTICAL spelling as `operator[]`: both panic on out-of-bounds in Rust, so C++'s throw-vs-UB distinction between them collapses to one Rust panic policy (documented UB refinement) |
    | `v.empty()` | `v.is_empty()` | -> `i1` |
    | `v.clear()` | `v.clear()` | void result |
    | `s.size()` / `s.length()` | `s.len()` then cast | identical convention to `v.size()` |
    | `s1 += s2` (`operator+=`, string operand) | `s1.push_str(&s2)` | deref coercion `&String`->`&str` applies at the argument position (mirrors `c_str()`'s printf borrow and `__emitrust_sprintf`'s staged String borrow) |
    | `s += "literal"` (`operator+=`, literal operand) | `s.push_str("literal")` | no borrow needed for a `&'static str` |
    | `s += 'c'` (`operator+=`, integer/char operand) | `s.push(c as char)` | reuses `wrapCharFormat`/`__emitrust_fmt_c`, the SAME ASCII-only conversion printf's `%c` already uses |
    | `s.empty()` | `s.is_empty()` | -> `i1` |
    | `s.c_str()` fed DIRECTLY to printf's `%s` | a shared borrow `&s` | recognized in `emitPrintfStringArg` (the SAME function `%s`'s literal/char-array/cstr-helper shapes already go through), reusing the FILE*/`%s` machinery; `.c_str()` is recognized in NO OTHER position — anywhere else (assigned to a local, passed to a non-printf call, ...) is a located rejection |

  `v[i]`/`v.at(i)` are PLACE operations (`emitStlVectorIndexPlace`
  builds an `emitrust.subscript` over the receiver, reconstructing the
  element type via `parseStlElementType`), not value operations — real
  C++ `operator[]`/`at()` both return `T&`. A scalar VALUE read (`int x
  = v[i];`) therefore reaches this through `emitLValue`'s own NEW
  `CXXOperatorCallExpr`/`CXXMemberCallExpr` cases (`ImportC.cpp`): the
  implicit `CK_LValueToRValue` cast wrapping the call expression calls
  `emitLValue` on the call itself, exactly as it would for any other
  reference-returning place. `emitCall`'s own `CXXOperatorCallExpr`/
  `emitStlMemberCall`'s `at()` branch (reached for a discarded
  STATEMENT-position use, `v[i];` with no consuming cast) load the SAME
  place — one `emitStlVectorIndexPlace` helper, two consumption
  contexts. `v[i] = x` (assignment TARGET) is NOT supported this wave —
  read position only.

  Two small AST-tolerance fixes the STL fixtures surfaced, both
  generically safe for the C path (neither AST shape a C construct ever
  produces) and applied in `emitRValue`'s top-level dispatch: (1) a
  prvalue binding to a by-value/rvalue-reference parameter
  (`v.push_back(1)` binding the literal `1` to `push_back(T&&)`) is
  wrapped in a `MaterializeTemporaryExpr`, unwrapped like
  `ExprWithCleanups`; (2) an lvalue bound to a C++ REFERENCE parameter
  (`push_back(const T&)` binding an existing variable, `v.push_back(seed)`)
  is left as a BARE `DeclRefExpr` with no `CK_LValueToRValue` wrapper at
  all (binding a reference does not "read" the value) — where C always
  wraps a scalar value use in that cast — so a new fallback loads the
  referenced place directly. This fallback's scope is provably narrow:
  W2.0 already rejects every USER-DEFINED reference PARAMETER type
  outright (`"unsupported: reference types are not yet supported"`), so
  the only reference-bound arguments that can ever reach it are this
  wave's own STL call sites.

  **UB-refinement stance** (documented, not merely implied): C++
  `operator[]` out-of-bounds access is undefined behavior; Rust's `[]`
  indexing panics deterministically. `.at()` bounds-checks and throws in
  C++; mapping it to the SAME panicking `[]` spelling as `operator[]`
  collapses two distinct C++ behaviors (UB vs. a catchable exception)
  into ONE deterministic Rust behavior (an uncatchable panic) — a
  refinement in the same family as this transpiler's existing
  UB-refinement policies (unsigned wraparound via `wrapping_*`,
  `abs(INT_MIN)`, ...): every INPUT behavior the refinement changes was
  already undefined or program-terminating in the C++ source, so no
  program that previously had defined, distinguishable behavior changes
  meaning.

  **OUT** this wave, located rejections (or, for ranged-for, an
  UNCHANGED pre-existing generic rejection — tracked for a later wave):
  every other STL container (`std::map`, `std::set`, `std::array`,
  `std::pair`, `std::optional`, ...); iterators and algorithms
  (`begin()`/`end()`, `<algorithm>`); streams (`std::cout`/`cin`/
  `stringstream`); the sized/fill vector constructor
  (`std::vector<T>(n)`); an initializer-list constructor
  (`std::vector<T>{1,2,3}`); copy/move construction; every vector method
  beyond the table above (`pop_back`, `back`, `front`, `insert`,
  `erase`, `reserve`, `capacity`, `resize`, `data`, iterators, ...);
  every string method beyond the table above (`operator[]` — Rust's
  `String` has no `Index<usize>` impl, only `[T]`/`Vec<T>` do, so bytes
  indexing would need a DISTINCT "index into `as_bytes()`" spelling this
  wave does not implement — `find`, `substr`, `append`, `insert`,
  `erase`, `compare`, `data`, iterators, ...); `operator+=` with a
  runtime (non-literal) `const char*` right-hand side; ranged-for over a
  vector (`CXXForRangeStmt` has no statement-import case AT ALL —
  deferred, not merely unrecognized-method rejected — so it falls
  through the pre-existing generic statement dispatch exactly like
  W2.0's `try`/`catch` baseline: `"unsupported statement:
  CXXForRangeStmt"`); `std::vector`/`std::string` as a function parameter
  or return type (untested and unexercised this wave, though nothing
  specifically blocks the TYPE from mapping there — only construction,
  the method table, and locals were built and pinned). Fuzz: C++ STL
  coverage is DEFERRED (`test/Fuzz/genprog.py` is a pure-C generator; a
  templated C++ generator variant is a nice-to-have, not attempted this
  wave to avoid scope creep) — the 150-seed campaign gated this wave is
  the SAME pure-C-path regression check every C++ wave has run,
  unaffected by (and not exercising) any of this wave's code.

  Gates: warning-free build; `check-emitrust` 299/299 (294 pre-existing +
  5 new: `test/Import/Cpp/stl-vector.cpp`, `stl-string.cpp`,
  `stl-invalid.cpp` (split-file, 12 located rejections), `test/EndToEnd/
  stl-vector.cpp`, `stl-string.cpp` — the last two byte-identical against
  `clang++ -std=c++17`, each threading distinct literals through two
  independently-seeded calls so a wiring bug could not hide behind a
  repeated value); c-testsuite ledger unchanged at exactly 220/220/0/0;
  a byte-identical `--emit=rust` snapshot over every pre-existing
  `test/EndToEnd/*.c` file (captured before and after this wave's `lib/`
  changes — every new/widened code path is either gated on
  `isInStdNamespace()` — unconditionally false for C — or dead code for
  an AST shape only C++ produces); a 150-seed differential fuzz campaign
  (seeds 10200-10349, `--range-check`), 150/150 passed, 0 miscompiled, 0
  range violations, 100% oracle agreement (a pure C-path regression
  check, per the Fuzz OUT note above).
  OUT of scope for this wave, staying rejected (tracked for later
  waves): everything in the OUT list above.
  (test/Import/Cpp/stl-vector.cpp, stl-string.cpp, stl-invalid.cpp;
  test/EndToEnd/stl-vector.cpp, stl-string.cpp)

## Track 5 Third-party validation (external demand signal)

Track 4's corpus is authored by this project. That has now produced two
demonstrated blind spots in one week -- every program was written with a
`main`, which hid the fact that LIBRARY projects were unreachable under any
flag (FR-51); and the paper harness enumerated multi-TU tests as single files,
which manufactured six spurious repair-search "rescues" (recorded under
FR-52). A self-authored benchmark encodes its authors' assumptions twice, once
in the code and once in the harness, and neither encoding is visible from
inside.

Track 5 therefore measures UNMODIFIED THIRD-PARTY C, cloned at pinned commits
and compiled with the project's OWN flags (via `cmake
-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` feeding FR-45's `--compdb` wherever the
upstream build supports it). Selection within a repository must be MECHANICAL
and stated -- every `.c` in a directory, or the compile database wholesale --
because hand-picking the files that happen to work would reproduce exactly the
defect this track exists to correct.

**The measurement separates two failure kinds, and conflating them would make
the result meaningless:**
 - `PARSE_FAIL` -- clang could not build an AST at all (a missing config
   header, an absent target define). This is a CONFIGURATION limitation of the
   harness, and says nothing about the supported subset. How much
   configuration a real codebase needs before the tool can even look at it is
   itself a product finding, not a footnote.
 - `REJECT` -- parsed cleanly, but could not be translated. This is the real
   demand signal, and the ranked root-blocker tally over it (FR-49) is what
   should sequence work after this track.

Outcome vocabulary per unit: `TRANSLATED_FULL` (crate builds, zero stubbed,
zero dropped), `PARTIAL` (crate builds, some stubbed or dropped), `NO_CRATE`,
`PARSE_FAIL`.

Initial target set, chosen to span best case to realistic case rather than to
flatter: small self-contained libraries (cJSON, heatshrink, tinycbor); RTOS
and networking cores (FreeRTOS-Kernel, lwIP `src/core`); and crypto/DSP
(mbedTLS `library/`, tinycrypt, CMSIS-DSP). CMSIS-DSP is the deliberate best
case -- large amounts of pure integer and fixed-point math, close to the
supported subset -- and mbedTLS the realistic one; the GAP between those two
is the most informative number the track can produce.

**RESULTS (2026-07-31). 289 per-unit measurements across 11 repositories;
288 parsed.**

```
                     units parsed  full partial no-crate builds  items ported
mbedtls                163   163     0      77       31     30   3976/8415 47%
lwip src/core           69    69     1*     34        5     30   1139/2354 48%
tinycrypt               32    32     0      15        0      5     66/177  37%
FreeRTOS-Kernel         31    31     0       6        3      6      61/254 24%
CMSIS-DSP               92    92     0       0       91      0        0/0   0%
tinycbor                13    13     0       0       13      0        0/0   0%
cJSON / heatshrink /
  nanopb / tiny-AES     34    33     1*      8        4     10      70/385 18%
TOTAL                  289   288     2*    143      141     85  5328/11773
```

`*` BOTH `TRANSLATED_FULL` results are VACUOUS -- empty translation units
behind a disabled `#ifdef` (`graph_items=0`, a two-line crate). **The real
count of third-party translation units translated in full is ZERO.**

Findings, in order of how much they should change what happens next:

1. **`PARSE_FAIL` = 1 of 289.** Every project's own compile database was
   consumed successfully and clang built an AST for essentially everything.
   There are no configuration excuses in this data; every number below is a
   genuine translation limit. Configuration effort was also LOWER than
   expected -- both FreeRTOS and lwIP ship usable config headers, and the
   measurement authored none, only ~36 lines of build glue.

2. **The item fraction flatters. By KIND, functions are 1-6%.** lwIP reads
   48% of items only because enums (100%) and records (45-63%) translate well
   while functions do not: FreeRTOS 5/129 (3.9%), lwIP 45/714 (6.3%), nanopb
   1/81 (1.2%). Nor is `ported` transitive -- walking the emitted call graphs,
   STUB-FREE functions are 3/39, 26/88 and 1/5. The 39 functions with real
   bodies are byte-swappers, config-empty inits and one-line wrappers. No
   queue operation, no scheduler function, no TCP state-machine function.

3. **A pointer stored in a struct field or a global is the dominant
   construct.** `rejected-type-cascade` alone is 51.6% of the corpus-wide
   root tally (3384 of 6553), and it is a cascade from that root:
   one rejected struct disqualifies every type and function naming it. The
   roots are each project's central types -- `netif`, `pbuf_custom`,
   `stats_`, the `xSTATIC_*` FreeRTOS types, `pb_callback_s`, mbedTLS's
   context structs. This is **C99-43**, the single remaining unchecked box on
   the C99 roadmap.

4. **Dynamic memory is NOT the barrier: 4 occurrences in 289 units (0.06%),
   and twice in the whole of lwIP.** This is the sharpest correction the
   third-party data delivers. FR-39 and the container/fat-op work invested
   substantially in modelling `malloc` because the SELF-AUTHORED corpus
   ranked `dynamic-memory` as its top blocker twice. Real embedded C
   allocates statically and threads pointers through structs. The demand
   signal was measuring its own authors.

5. Defects the run exposed, each reproduced in a handful of lines and fixed
   or filed: the Pass-A planner recovery hole (FR-53), the canonicalized
   unsigned-op legalization gap (FR-54), and two classes of emitted crate
   that `rustc` rejects (FR-55). Plus, still open: a SEGFAULT on lwIP's
   121-TU target (no diagnostic, deterministic); NON-TERMINATION of
   `--incremental` on two FreeRTOS files where strict mode finishes in
   seconds; and `--compdb` refusing a GCC-produced compile database
   (`-Wlogical-op` reaches clang as `-Werror,-Wunknown-warning-option`),
   which is the first thing a real embedded user would hit since almost every
   such project ships a gcc build.

6. **FR-41's colouring is not predictive on third-party code.** CMSIS-DSP
   colours 100% green and ports 0%; mbedTLS colours 99.6% green and ports
   47%. The CONTRACT holds -- these are false greens, the deliberately safe
   direction, and false reds remain zero -- but the ARGUMENT does not. FR-43's
   permissive-unsoundness rationale was that a false green costs one probe
   because the search repairs it; the search repairs nothing here. On a
   self-authored corpus the colouring looked exact; on real code it is 50-100
   points out with no repair behind it.

**CORRECTION (2026-07-31, same day).** The first aggregation of this track
reported 434 units and a 73%/52% blocker split. Both were inflated by a
defect in the AGGREGATOR, not in the measurements. It globbed
`thirdparty_*.csv` and excluded only `_blockers.csv` by NAME, so the
per-diagnostic tally files (137 rows) and the project-level aggregate rows
were counted as translation units; and its blocker sum matched
`*_blockers.csv` against a group that ships BOTH a direct and a root tally,
double-counting it. Corrected: **289 distinct per-unit measurements**,
`rejected-type-cascade` **51.6%** of 6553 root-tagged rejections,
`dynamic-memory` **4**. The aggregator now selects per-unit files by SCHEMA
(the presence of an `outcome` column) rather than by filename. A further
correction: the "functions are 1-6%" figure holds for the RTOS/networking
group only -- mbedTLS is 36.1% and tiny-AES-c 50.0% by function. None of the
qualitative findings change; the numbers do, and the wrong ones were
reported before this note.

**PREMISE REFUTED (C99-43 spike, 2026-07-31).** The conclusion above --
"73% of rejections reduce to a pointer stored in a struct field or global,
therefore C99-43" -- was derived from the blocker TAG
(`rejected-type-cascade`) without checking what the cascade roots actually
were. A spike measured them, and **a pointer stored in a struct field is
ALREADY SUPPORTED**: `mapStructFieldType` maps every single-level
non-function pointer field -- including `void *`, `char *` and `T **` -- to a
plain `i64` cursor and never fails. Verified directly: `struct A { struct A
*next; }`, `{ void *payload; }`, `{ char *name; }`, `{ int **pp; }` and
`{ void (*cb)(int); }` all import cleanly today.

What rejects is a pointer type in a **composite type position**, all four
shapes funnelling into one residual in `mapType`. Over 80 deduped root record
rejections:

| root cause | records | share |
|--|--:|--:|
| fn-ptr field whose signature names a pointer | 47 | 58.8% |
| union with a pointer arm | 9 | 11.2% |
| union arm cannot alias the storage slot (**not a pointer problem**) | 8 | 10.0% |
| array-of-pointer field | 7 | 8.8% |
| other (volatile, fn-ptr pointer result, nested) | 9 | 11.2% |

And the survey's #1 root is not a pointer problem at all: **`netif` fails on
`ip_addr`'s `union { ip6_addr_t; ip4_addr_t; }`** -- the C99-44 one-slot
union model. `netif` + `ip_addr` alone are 258 cascade-blocked items, 27.5%
of all cascade damage in the corpus, from one union of two small structs.

**The ownership census is the decisive table.** Of the 661 pointer fields the
importer ALREADY accepts: 48.5% store a pointer received from a caller (a
borrow needing a lifetime), 19.8% are never written, 17.7% copy another
pointer, and only **6.4%** are the allocation-or-array-element shapes that
index handles and arenas address. FR-37/38/39 -- the direction this project
has been extending -- covers 6.4% of real pointer fields. Lifetimes would
cover the 48.5%, and would break the `Copy + Default` struct invariant the
whole dialect rests on.

**Recommendation: CASCADE CONTAINMENT, not pointer translation.** Emit a
rejected composite-position field as a placeholder (and array-of-pointer as
`[i64; N]`, which is the cursor convention already used for scalar pointer
fields), keeping the record importable, and reject at the ACCESS site. This
translates no new pointers. Measured over the 212 TUs reporting in both
configurations: records dropped 117 -> 5 (-96%), cascade-blocked items
705 -> 29 (-96%), items ported +18.9%, **functions ported 8.3% -> 11.7%
(+37.7%)**. The precedent is already in the tree (`unionByteArrayArms`, the
FAM field omission): a type-level concession with rejection deferred to use.

Three findings that must travel with it:
 - **43 TUs stop emitting a crate**, because containment makes previously
   cascaded structs import and thereby makes PRE-EXISTING non-recoverable
   failures reachable -- 39 are `extern global variable not defined in any
   translation unit`, 2 the FR-42 recovery non-termination, 2 legalization.
   Stage 1 must not ship without fixing those first.
 - **The union placeholder is NOT ready**: it silently emitted invalid Rust
   with no diagnostic in its first cut, and even guarded it breaks 42 crates
   with `E0609` while buying 16 items. Rejected.
 - **FR-41's doctrine becomes false.** `ItemColoring.h`'s claim that "a
   missing TYPE cannot be replaced… so type-poisoning is transitive" is
   exactly what containment refutes. The colouring and FR-49's root
   attribution would model a cascade that no longer happens. This is also
   Contribution 1 of the accompanying paper, and needs qualifying there.

**Revised priority.** Not C99-43. In order: cascade containment (Stage 1,
gated on the enabling fixes), then fn-ptr components through the parameter
mapper (Stage 2, which turns 47 of 80 roots into working callbacks), then the
UNION model (Stage 3 -- 30.7% of the cascade and the real `netif` blocker,
and a C99-44 question, not a pointer one). Index handles and lifetimes: never,
on the measured 6.4% and the invariant break respectively.

**Track 5 re-measured after FR-53/54/55 (same pinned SHAs, same compile
databases, same harness).** The three defects the first run exposed were
fixed and the affected repositories re-measured:

| | before | after |
|--|--|--|
| small libs: units emitting NO crate | 14 of 22 | **2 of 22** |
| small libs: crates that `cargo build` | 8 | **20** |
| small libs: ported items | 52 | **169** |
| CMSIS-DSP: units emitting a crate | 0 of 91 | **83 of 91** |
| tinycrypt + tiny-AES: emitted crates building | 6 of 25 | **25 of 25** |

The small-libs ported FRACTION falls 18.9% -> 12.0% while absolute ported
items more than triple, because the denominator grew fivefold: the twelve
rescued units contribute their entire item graphs, which previously counted
as nothing at all. A fraction that falls because a benchmark stopped hiding
its failures is the honest direction, and it is recorded here rather than
quietly replaced by the absolute count.

**The FR-42 caveat was wrong and is corrected.** It claimed planner
rejections "cannot be attributed to a single droppable item". All seven
`emitError` sites in the two rejecting planners carry a location inside one
declaration, and FR-53 attributes every one. The claim survived because
nothing in the self-authored corpus exercised it -- the same failure mode as
the `main`-only blind spot and the harness mis-invocation, and the third time
a documented caveat turned out to be the dominant real-world behaviour.

**Recovery still stops at the IMPORT boundary, and that is now the leading
blocker.** The two units that still yield nothing fail AFTER the declaration
walk: `cannot translate non-finite floating-point constant` from the Rust
emitter, and `failed to legalize operation 'scf.if'` from the conversion
pipeline. A single un-legalizable operation costs the entire crate exactly
the way a planner rejection used to. FR-42's per-item recovery has no
counterpart in the pass pipeline or the emitter; giving it one is the natural
successor to FR-53.


## Track 4 RealWorld corpus (demand signal)

`test/RealWorld/` is a corpus of small, realistic, deterministic C programs
whose job is to GENERATE DEMAND — not to be a conformance target. The fixed
c-testsuite (220/220) no longer forces new work, so the remaining deep designs
(C99-43 ptr-to-ptr, C99-46 dynamic memory, and the pointer-model extensions)
are ranked by how often real programs actually hit them, measured, rather than
by speculation. The corpus need not be cleared; a program that stays rejected
is a standing backlog item, and one that starts transpiling (after a deep-design
wave) ratchets into the transpiled set as a regression guard and runtime perf
workload.

**Harness.** `test/RealWorld/run_realworld.py` (modeled on the c-testsuite
ledger runner, `test/CTestSuite/run_c_testsuite.py`) drives each program
through `emitrust-cc --emit=crate --build`. A program is a single top-level
`Inputs/<name>.c` (one TU) or an `Inputs/<name>/` subdirectory whose `*.c`
compile together (multi-TU). Outcomes: **REJECTED** (a located diagnostic, rc≠0
— the demand signal, tagged by blocker category), **TRANSPILED** (the crate
built and its stdout matched a `clang -std=c11` native build of the same
sources — the differential oracle, the same philosophy as the EndToEnd
differentials), or **MISCOMPILE** (built but crashed / exited non-zero /
diverged from native — always fails, quarantine aside). A two-way ratchet
against `expected-transpile.txt` (programs expected to transpile) plus an
(empty) `known-miscompiles.txt` quarantine mirrors the ledger; the runner also
prints a **blocker-tag tabulation** — the survey signal W4.1 reads. Gated in
`ninja check-emitrust` via the `test/RealWorld/realworld.c` lit stub
(`REQUIRES: cargo`); the corpus lives under `Inputs/` so it is excluded from
lit discovery (`config.excludes = ["Inputs"]`) yet regression-protected through
the stub. `argv` VALUES are dropped at import (the main wrapper passes only
`argc`), so command-line-argument programs reject.

**Blocker tags** are a heuristic over the first diagnostic line: the
system-header symbol is parsed out (`free`/`realloc`/`malloc`/`calloc` →
`dynamic-memory`, else `libc:<name>`); the shared "pointer assigned a
non-address value" / "no known target object" wording is refined by reading the
cited source line (an alloc call → `dynamic-memory`, `strchr`/`strrchr` →
`strchr-result-bind`, else `pointer-local-nonaddress`); a compiler crash
("PLEASE submit a bug report" / "Stack dump") tags `crash`.

**W4.0 snapshot (13 programs): 5 transpiled, 8 rejected, 0 miscompiled.**
Transpiled (pinned regression guards / perf workloads): `base64`, `calc`
(two-TU), `logger` (two-TU), `sieve`, `word-count`. Rejected, by blocker
frequency: **`dynamic-memory` ×2** (`linked-list`, `malloc-stack` — local
`malloc` + `free`, C99-46), then one each of `returned-pointer` (`binary-tree`,
C99-43), `strchr-result-bind` (`grep-lite`), `self-ref-pointer-member`
(`union-find`, C99-43), `global-string-cursor` (`expr-eval`), `argv`
(`argv-echo`, C99-43 / argv-values-dropped), and **`crash` ×1** — `crc32`
SEGFAULTS the importer when a `const char *` VARIABLE pointing into a string
literal is passed to a subscripted (slice) parameter: `emitBorrowArgument`
resolved a base-less literal-backed pointer and fell through every base-keyed
guard to a null-base error branch that dereferenced `pointer->base->getName()`
(the `(unsigned char)` cast is a red herring — `crc32("literal", n)` and an
array argument both already transpile; the trigger is the const-`char*`
variable into a literal × slice parameter). The crash is
a robustness bug (the importer must emit a located rejection, never a segfault);
it is pinned here as the highest-priority survey finding and mapped to a
follow-up fix wave, not fixed in the test-only W4.0. W4.1 tabulates and ranks
these to drive W4.2+ (the ranking overrides the plan's pre-baked ladder order).

**W4.2 update: `union-find` cleared.** The owner-struct self-reference
extension (FR-37/FR-38, six stages) resolved the `self-ref-pointer-member`
blocker entirely: `union-find` now TRANSPILES and differentially matches a
clang-native build byte-for-byte (`test/RealWorld/expected-transpile.txt`
ratchet-updated forward; verified live via `run_realworld.py`:
`total=13 transpiled=6 rejected=7 miscompiled=0`, zero regressions/
improvements against the manifest). The `self-ref-pointer-member` tag is
retired — no remaining rejected corpus program carries it. `binary-tree`
(`returned-pointer`) was separately re-checked after this work landed and
still rejects, unchanged, at the same `insert` call site: the returned
pointer there roots in a `malloc`'d node (a callee-local, non-array-backed
allocation), which the array-rooted owner-index-return mechanism (FR-36)
does not and cannot cover — the blocker is dynamic memory (C99-46), not
the returned-pointer machinery itself. Current tally: **6 transpiled, 7
rejected, 0 miscompiled** — `dynamic-memory` ×2 (`linked-list`,
`malloc-stack`), and one each of `returned-pointer` (`binary-tree`,
now confirmed malloc-rooted), `strchr-result-bind` (`grep-lite`),
`global-string-cursor` (`expr-eval`), `argv` (`argv-echo`), and `crash`
(`crc32`, still unfixed, still the highest-priority robustness finding).

**W4.1 ranked survey.** The corpus rejections, ranked by
programs-unblocked-per-cost (RFC-gated features sink; a robustness crash
floats to the top regardless of feature value). This ranking OVERRIDES the
plan's pre-baked ladder order — the demand signal, not speculation, sequences
the remaining deep designs. `self-ref-pointer-member` is retired (union-find
transpiles); seven rejected programs remain.

| Rank | Blocker | Programs | RFC? | Cost | Next wave |
|--|--|--|--|--|--|
| 1 | crash | 1 (`crc32`) | no | low | **RESOLVED** — literal-backed slice-argument fix (W4.1 commit 2) |
| 2 | dynamic-memory | 2 (`linked-list`, `malloc-stack`) | no | med | W4.2 ladder a (local const-size malloc + free) |
| 3 | strchr-result-bind | 1 (`grep-lite`) | no | med | bind a strchr result to a pointer local |
| 4 | global-string-cursor | 1 (`expr-eval`) | no | med | global `char*` into a literal, walked as a cursor |
| 5 | returned-pointer | 1 (`binary-tree`, ALSO dynamic-memory) | likely | high | pointer to a heap object |
| 6 | argv | 1 (`argv-echo`) | — | high | W4.3 argv cursor table |

Rationale: the `crc32` crash is a robustness override — a compiler must never
segfault, and the fix is cheap and localized, so it precedes the entire feature
ladder. `dynamic-memory` clears the most single-blocker programs (2) at moderate
non-RFC cost, so it leads the ladder (W4.2). `binary-tree` is DOUBLE-blocked
(returned-pointer AND dynamic-memory — its returned pointer roots in a
`malloc`'d node, not an array), so it will not clear until both land; it sits at
the RFC-likely tail. `argv` needs the second-order cursor-table generalization
(W4.3). Ranks 2–6 are future waves; only rank 1 is actioned this session.

**W4.1 update: `crc32` crash fixed.** `emitBorrowArgument` gained a
`literalBacking` branch (right after `emitPointerRValue`, before any base-keyed
path): a `const char *` variable pointing into a string literal passed to a
subscripted slice parameter now reslices the literal's backing rather than
crashing. A subscripted `const char *` parameter is a MUTABLE slice
(`!emitrust.mut_ref<!emitrust.slice<i8>>`) — so the fix rematerializes a fresh
mutable backing of the literal at the call site (mirroring the direct-literal
argument path `f("abc", n)`; writing through a pointer to a string literal is
UB, so the per-call copy is unobservable), while a shared byte-slice parameter
borrows the const backing read-only. (This is a deliberate refinement of the
W4.1 plan, which assumed the parameter was a shared const ref and would
"reject if mutable"; the parameter is in fact mutable, and rejecting it would
have kept `crc32` blocked, so the sound copy path is used instead.) `crc32`
now TRANSPILES and matches a `clang -std=c11` native build byte-for-byte;
the manifest ratcheted forward to include it. RED→GREEN pin:
`test/Import/C/pointers-param-literal-slice.c`. The branch fires only for the
previously-crashing literal-into-slice shape, so it perturbs no existing
output. **Current tally: 7 transpiled, 6 rejected, 0 miscompiled** — transpiled
add `crc32` to `base64`/`calc`/`logger`/`sieve`/`union-find`/`word-count`;
rejected are `dynamic-memory` ×2 (`linked-list`, `malloc-stack`), and one each
of `returned-pointer` (`binary-tree`), `strchr-result-bind` (`grep-lite`),
`global-string-cursor` (`expr-eval`), and `argv` (`argv-echo`). The `crash`
tag is retired.

**W4.2e update: `dynamic-memory` resolved (rank 2).** The two
`dynamic-memory` programs both TRANSPILE now (FR-39, C99-46 Stage 1):
`malloc-stack` via the Part A local flat buffer, `linked-list` via the
Part B index-handle node pool. The `dynamic-memory` blocker tag is retired
from the corpus — no remaining rejected program carries it. `binary-tree`
stays rejected but as `returned-pointer` ONLY (a returned `malloc`'d node
dangles; the pool model deliberately keeps it out — see FR-39). **Current
tally: 9 transpiled, 4 rejected, 0 miscompiled** — transpiled add
`malloc-stack` and `linked-list`; the four rejected are `returned-pointer`
(`binary-tree`), `strchr-result-bind` (`grep-lite`), `global-string-cursor`
(`expr-eval`), and `argv` (`argv-echo`). Remaining ranked demand (rank 3+):
strchr-result-bind, global-string-cursor, returned-pointer (RFC-gated,
W4.5), and argv (W4.3).

**Csmith DEFERRED** (documented decline): the flake toolchain is off-limits
this cycle, so no new generator dependency is added. The seeded differential
fuzzer (`test/Fuzz`, generator v5) plus this hand-authored corpus are the
differential coverage; a Csmith leg — which would need a flake input — is
future work if the fuzzer's coverage gaps demand it.

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

## Performance baseline

Every EndToEnd test's *runtime* is trivial (a handful of loop iterations or
printf calls), so the cost that matters is transpile and transpile+build
wall time, not generated-binary runtime. `tools/bench-transpile.sh` measures
both (bash `time`, no hyperfine dependency) over a fixed set of ten
representative EndToEnd programs. The recorded baseline, reproduction
command, and full per-program table live in
[docs/perf-baseline.md](docs/perf-baseline.md); in short, transpile-only is
~0.4s total across all ten programs while transpile+build is ~8.4s total —
`cargo build --release` dominates, consistent with W1.0's finding that the
full 220-test ledger's ~20s wall time is cargo-dominated. This is the
regression reference for the upcoming ImportC.cpp split and future waves.
