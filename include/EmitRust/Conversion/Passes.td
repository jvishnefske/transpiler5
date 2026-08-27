//===- Passes.td - EmitRust conversion passes --------------*- tablegen -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
// Defines the conversion passes that lower core MLIR dialects (ub, arith,
// func, scf) into the EmitRust dialect, and the composite convert-to-emitrust
// pass that applies all of the pattern sets under one partial conversion.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CONVERSION_PASSES
#define EMITRUST_CONVERSION_PASSES

include "mlir/Pass/PassBase.td"

//===----------------------------------------------------------------------===//
// UBToEmitRust
//===----------------------------------------------------------------------===//

def ConvertUBToEmitRust : Pass<"convert-ub-to-emitrust"> {
  let summary = "Convert UB dialect to EmitRust dialect";
  let description = [{
    Converts `ub.poison` operations into `emitrust.constant` operations
    holding the zero/default value of the result type: `0` for integer and
    index types, `false` for `i1`, and `0.0` for floating-point types. The
    EmitRust MVP has no notion of undefined values, so poison collapses to a
    deterministic default. Poison values of any other type are left illegal
    and fail the conversion.
  }];
  let dependentDialects = ["::mlir::emitrust::EmitRustDialect"];
}

//===----------------------------------------------------------------------===//
// ArithToEmitRust
//===----------------------------------------------------------------------===//

def ConvertArithToEmitRust : Pass<"convert-arith-to-emitrust"> {
  let summary = "Convert Arith dialect to EmitRust dialect";
  let description = [{
    Converts scalar Arith operations into their EmitRust counterparts:

    - `arith.constant` (integer, index, and float scalars) to
      `emitrust.constant`.
    - `arith.addi`/`subi`/`muli`/`divsi`/`remsi` and
      `arith.addf`/`subf`/`mulf`/`divf` to
      `emitrust.add`/`sub`/`mul`/`div`/`rem`.
    - `arith.cmpi` with the signed predicates `eq`, `ne`, `slt`, `sle`,
      `sgt`, `sge` and `arith.cmpf` with the ordered predicates `oeq`,
      `one`, `olt`, `ole`, `ogt`, `oge` to `emitrust.cmp` with the
      predicates `eq`, `ne`, `lt`, `le`, `gt`, `ge`.
    - `arith.extsi`, `arith.trunci`, `arith.sitofp`, `arith.fptosi`, and
      `arith.index_cast` to `emitrust.cast`.

    Unsigned operations (`divui`, `remui`, unsigned `cmpi` predicates,
    `extui`, ...) and unordered float comparisons are deliberately not
    supported in the MVP; they stay illegal so the conversion fails with the
    standard "failed to legalize" diagnostic.
  }];
  let dependentDialects = ["::mlir::emitrust::EmitRustDialect"];
}

//===----------------------------------------------------------------------===//
// FuncToEmitRust
//===----------------------------------------------------------------------===//

def ConvertFuncToEmitRust : Pass<"convert-func-to-emitrust", "ModuleOp"> {
  let summary = "Convert Func dialect to EmitRust dialect";
  let description = [{
    Converts Func operations into their EmitRust counterparts:

    - `func.func` to `emitrust.func`, converting the signature through the
      type converter and inlining the body region. Functions with more than
      one result are not convertible.
    - `func.return` to `emitrust.return`.
    - `func.call` to `emitrust.call_opaque` with the callee symbol name as
      the opaque string callee.
  }];
  let dependentDialects = ["::mlir::emitrust::EmitRustDialect"];
}

//===----------------------------------------------------------------------===//
// SCFToEmitRust
//===----------------------------------------------------------------------===//

def ConvertSCFToEmitRust : Pass<"convert-scf-to-emitrust"> {
  let summary = "Convert SCF dialect to EmitRust dialect, maintaining "
                "structured control flow";
  let description = [{
    Converts structured control flow into EmitRust statements. SSA results
    of the SCF operations become mutable `emitrust.let` bindings assigned
    from within the inlined regions:

    - `scf.if` with results becomes default-initialized mutable let
      bindings followed by a result-less `emitrust.if` whose regions assign
      the yielded values into the bindings; the `scf.if` results are
      replaced by the binding values. A result-less `scf.if` becomes a
      plain `emitrust.if`.
    - `scf.while` becomes mutable let bindings for the carried values and
      the results, and an `emitrust.loop` that evaluates the before-region,
      exits via `emitrust.if` + `emitrust.break` when the condition is
      false (assigning the result bindings first), and runs the
      after-region, assigning the carried bindings at `scf.yield`.
    - `scf.for` becomes mutable let bindings for the iteration arguments
      and an `emitrust.for` whose body assigns the bindings at `scf.yield`.
    - `scf.index_switch` becomes default-initialized mutable let bindings
      and an `emitrust.switch` over the same discriminator and case
      values, with the case and default regions inlined into the
      corresponding switch regions; the regions assign the bindings at
      their `scf.yield`.
  }];
  let dependentDialects = ["::mlir::emitrust::EmitRustDialect"];
}

//===----------------------------------------------------------------------===//
// ConvertToEmitRust
//===----------------------------------------------------------------------===//

def ConvertToEmitRust : Pass<"convert-to-emitrust", "ModuleOp"> {
  let summary = "Convert core MLIR dialects to the EmitRust dialect";
  let description = [{
    Composite conversion that populates the ub, arith, func, and scf
    pattern sets and applies them under a single partial conversion. The
    EmitRust dialect and `builtin.module` are legal; the arith, cf, func,
    memref, scf, and ub dialects are illegal, so any operation from those
    dialects that survives the conversion fails the pass with the standard
    "failed to legalize" diagnostic.
  }];
  let dependentDialects = ["::mlir::emitrust::EmitRustDialect"];
}

//===----------------------------------------------------------------------===//
// EmitRustCanonicalRoundTrip
//===----------------------------------------------------------------------===//

// Note: like the two differential checkers below, this is a verification
// (analysis-only) pass rather than a conversion; it lives here because this is
// the project's only pass TableGen file.
def EmitRustCanonicalRoundTrip
    : Pass<"emitrust-canonical-roundtrip", "ModuleOp"> {
  let summary = "Round-trip the module through MLIR's canonical generic form";
  let description = [{
    FR-134, the first slice of the optional canonicalization pipe. Prints the
    module to MLIR's GENERIC form, parses that text back into a fresh
    `MLIRContext` carrying the same dialect registry, re-prints the reparsed
    module, and reports a hard error unless the two texts are identical.

    The pass MUTATES NOTHING. It marks all analyses preserved and leaves the
    module exactly as it found it, so enabling it can never change emitted
    Rust -- it either passes silently or fails the compilation loudly. That is
    the whole contract: a verification stage, not a lowering stage.

    WHY THE GENERIC FORM AND NOT THE PRETTY ONE. The generic form is MLIR's
    canonical serialization: no custom assembly is involved, so a round-trip
    failure is a real defect in a type, an attribute, or a region structure
    rather than a cosmetic printer asymmetry. The distinction is not
    hypothetical -- the FR-134 spike found that upstream `cf.switch`'s CUSTOM
    assembly cannot round-trip a negative case value (it prints `-2` as
    `18446744073709551614`, which its own parser then rejects as "integer
    value too large"), while the same op's GENERIC form prints
    `case_values = dense<-2>` and round-trips cleanly. Checking the pretty
    form would report that upstream cosmetic bug as a project defect; checking
    the generic form reports what actually threatens correctness.

    Round-tripping IN PROCESS, from the live module, is deliberate for the
    same reason: it never routes the IR through the pretty text that the
    upstream bug corrupts.

    Placed FIRST in the pinned lowering pipeline, so what it certifies is the
    FRONT END's output -- the point where an importer bug is still
    attributable to the importer.
  }];
  let dependentDialects = ["emitrust::EmitRustDialect"];
}

//===----------------------------------------------------------------------===//
// EmitRustRangeRefinementCheck
//===----------------------------------------------------------------------===//

// Note: this is a verification (analysis-only) pass, not a conversion, but
// it lives in the conversion pass registry because it internally drives the
// convert-to-emitrust pipeline and this is the project's only pass TableGen
// file.
def EmitRustRangeRefinementCheck
    : Pass<"emitrust-range-refinement-check", "ModuleOp"> {
  let summary = "Differential integer-range check across convert-to-emitrust";
  let description = [{
    Re-derives integer value ranges (`IntegerRangeAnalysis` over
    `InferIntRangeInterface`) before and after the convert-to-emitrust
    pipeline and reports a hard error when the two ranges of one observable
    value cannot describe any common value.

    Input modes:

    - Primary: a plain module. The pass analyzes it, clones it, runs
      convert-to-emitrust on the clone in a nested pass manager, analyzes
      the clone, and compares.
    - Secondary: a module holding exactly two nested `builtin.module` ops
      tagged `emitrust.stage = "before"` and `emitrust.stage = "after"`.
      The pass skips its internal conversion and compares the pair
      directly.

    Observation points are matched by (enclosing function symbol,
    per-function occurrence index, operand index): the operands of
    `emitrust.call_opaque "print!"` calls and the operands of function
    return terminators (`func.return` / `emitrust.return`).

    Verdict semantics: only an EMPTY INTERSECTION is an error. The
    intersection is taken with `ConstantIntRanges`' paired unsigned
    (umin/umax) and signed (smin/smax) bounds, and a violation is reported
    only when it is empty under BOTH the unsigned and the signed
    interpretation. This is the deliberately conservative choice: each
    interpretation's bounds are sound individually, but a range constructed
    from only one interpretation carries an artificially widened complement
    (`fromSigned`/`fromUnsigned`), so requiring both intersections to be
    empty is immune to one-sided widening and can never flag a pair of
    sound ranges that share a value. A post-conversion range that merely
    fails containment in the pre-conversion range (e.g. a memory load that
    is TOP after conversion) is counted in a statistic but stays silent.
    An uninitialized or absent lattice value is treated as TOP (any value),
    which never intersects to empty.

    All EmitRust transfer functions model WRAP (two's-complement) overflow
    — unsigned arithmetic lowers to Rust `wrapping_*` — so defined
    wrap-around in either stage never produces a false positive.

    Diagnostic contract:

    ```
    error: range refinement violation: '<fn>' print operand <k> has
    pre-conversion range [..] disjoint from post-conversion range [..]
    error: range refinement violation: '<fn>' return value <k> has
    pre-conversion range [..] disjoint from post-conversion range [..]
    ```

    The pass never modifies the IR; on success the module round-trips
    unchanged.
  }];
  let dependentDialects = ["::mlir::emitrust::EmitRustDialect"];
  let statistics = [
    Statistic<"numContainmentFailures", "containment-failures",
              "Observation points whose post-conversion range is not "
              "contained in the pre-conversion range (not an error)">
  ];
}

//===----------------------------------------------------------------------===//
// EmitRustValueIdentityCheck
//===----------------------------------------------------------------------===//

// Note: like emitrust-range-refinement-check above, this is a verification
// (analysis-only) pass that lives in the conversion pass registry because
// it internally drives the convert-to-emitrust pipeline.
def EmitRustValueIdentityCheck
    : Pass<"emitrust-value-identity-check", "ModuleOp"> {
  let summary = "Differential concrete interpretation across "
                "convert-to-emitrust";
  let description = [{
    Concretely interprets every parameterless function before and after the
    convert-to-emitrust pipeline and reports a hard error when the two runs
    observe different values in execution order. This complements
    emitrust-range-refinement-check: the range checker proves range
    consistency but is blind to value-identity bugs whose wrong value still
    lies inside the pre-conversion range — in particular the lost-copy/swap
    class of SSA-destruction bugs, which this pass does catch.

    Input modes (shared scaffold with the range checker):

    - Primary: a plain module. The pass interprets it, clones it, runs
      convert-to-emitrust on the clone in a nested pass manager, and
      interprets the clone identically.
    - Secondary: a module holding exactly two nested `builtin.module` ops
      tagged `emitrust.stage = "before"` and `emitrust.stage = "after"`,
      compared directly with no internal conversion.

    Entry points are the functions that take no arguments; each is
    interpreted independently with a fresh environment and a generous
    per-entry-point step budget. Observation points, recorded in execution
    order across the whole run (callees included), are the operands of
    `emitrust.call_opaque "print!"` calls and the operands of function
    return terminators. The first divergence per entry point is reported as
    a located hard error (values in signed decimal):

    ```
    error: value identity violation: '<fn>' print operand <k> at
    occurrence <n> observed <a> pre-conversion but <b> post-conversion
    error: value identity violation: '<fn>' return value <k> at
    occurrence <n> observed <a> pre-conversion but <b> post-conversion
    ```

    Unsupported coverage is a SOFT SKIP, never an error: when either run
    of an entry point reaches anything outside the interpreted op set (an
    `emitrust.call_opaque` whose callee neither resolves to an interpreted
    module function nor is "print!", memory/place ops beyond scalar
    `emitrust.variable`/`assign`/`load`, cell/global/struct/float ops, an
    exhausted step budget, ...), that entry point is skipped silently and
    counted in the skipped-entry-points statistic. Poison policy: a
    `ub.poison` value is materialized as a poison marker that may flow
    through rebindings and stores, but any entry point that actually
    EVALUATES it — arithmetic, comparison, branching, or an observation —
    is soft-skipped; interpreting poison as the constant the conversion
    folds it to would be unsound in general.

    The pass never modifies the IR; on success the module round-trips
    unchanged.
  }];
  let dependentDialects = ["::mlir::emitrust::EmitRustDialect"];
  let statistics = [
    Statistic<"numSkippedEntryPoints", "skipped-entry-points",
              "Entry points skipped as unsupported coverage (not an error)">
  ];
}

//===----------------------------------------------------------------------===//
// EmitRustLowerExternalRequirements
//===----------------------------------------------------------------------===//

def EmitRustLowerExternalRequirements
    : Pass<"emitrust-lower-external-requirements", "ModuleOp"> {
  let summary = "Express unresolved externals as a Rust trait (FR-52)";
  let description = [{
    Turns the body-less `emitrust.func` declarations the C importer marked
    `emitrust.external_requirement` — symbols the project REFERENCES but no
    translation unit DEFINES — into one `emitrust.trait_def @Externals` and
    makes the code that needs them generic over it.

    Three edits, in this order:

    - the marked declarations become the trait's associated functions, in
      module order, and are then erased;
    - the transitive closure of their CALLERS is marked
      `emitrust.externals_generic = "Externals"`, so the Rust emitter renders
      each as `fn f<E: Externals>(...)`. A function that reaches no
      requirement is untouched and keeps the signature it always had;
    - every call is requalified: a call to a requirement becomes
      `E::<name>`, and a call to a closure member becomes `<name>::<E>`.
      Both `emitrust.call_opaque` (free functions) and
      `emitrust.method_call` (methods of an `emitrust.impl`) are rewritten,
      which is sound because the two share one flat namespace — a method's
      Rust name IS the callee symbol `convert-func-to-emitrust` put in the
      call.

    The pass is a NO-OP on a module with no marked declaration, which is
    every module a project without unresolved externals produces.

    Two shapes are hard errors rather than silent mis-emission: a module that
    already defines an item named `Externals` (the trait would clash) or one
    named `E` (a Rust type parameter SHADOWS a same-named type inside the
    generic item, so a signature mentioning it would silently change
    meaning).
  }];
  let dependentDialects = ["::mlir::emitrust::EmitRustDialect"];
}

//===----------------------------------------------------------------------===//
// EmitRustLowerContainers
//===----------------------------------------------------------------------===//

def EmitRustLowerContainers
    : Pass<"emitrust-lower-containers", "ModuleOp"> {
  let summary = "Lower the backend-agnostic container ops to a concrete pool";
  let description = [{
    Greedily lowers the high-level container ops
    (`emitrust.collection`/`collection_push`/`collection_at`) to the
    production array backend: a fixed `[T;CAP]` pool (an `emitrust.variable`)
    plus a rank-0 `memref` i64 free cursor initialized to 0.

    - `collection` becomes the pool variable and the cursor cell (stored 0).
    - `collection_push` loads the cursor, takes that index, stores `index + 1`
      back, and yields the index — the CAP slots are pre-defaulted by the
      array's default value, so "append" is just a cursor bump.
    - `collection_at` becomes an `emitrust.subscript` of the pool at the index.

    This reproduces the exact legacy shape the importer previously inlined at
    malloc-pool recognition time (FR-39). The pass runs first in the
    emitrust-cc pipeline, before mem2reg promotes the cursor cell, so all
    downstream lowering is byte-for-byte identical to inlining the pool
    directly. A growable `Vec<T>` backend is deferred to the heap RFC
    (design.md W4.5); the array pattern is the single production container
    path.
  }];
  let dependentDialects = ["::mlir::emitrust::EmitRustDialect",
                           "::mlir::arith::ArithDialect",
                           "::mlir::memref::MemRefDialect"];
}

//===----------------------------------------------------------------------===//
// EmitRustActorLift
//===----------------------------------------------------------------------===//

def EmitRustActorLift : Pass<"emitrust-actor-lift", "ModuleOp"> {
  let summary = "Lift planned actor clusters to struct-owned state (FR-62)";
  let description = [{
    FR-62 slice 4: rewrites a pipeline-stage module (pure emission dialect,
    the exact input of the Rust emitter) into the same-thread actor shape
    described by the discardable attributes the emitrust-cc driver attaches
    (see EmitRust/Conversion/ActorLift.h for the attribute contract and the
    architecture-seam rationale):

    - each certified actor gets a synthesized `emitrust.struct_def` whose
      fields are the actor's owned mutable globals (and lifted
      function-local static cells), plus an `emitrust.impl` holding the
      actor's arm functions rewritten to take the actor as a leading
      `!emitrust.mut_ref` receiver (`&mut self`);
    - `global_load`/`global_store` of an owned global become member place
      loads/assigns on the receiver; a `global_load` of a CONST global
      inside the impl becomes the opaque-constant path (the impl is a
      SymbolTable, so the module-level symbol is unreachable there);
    - cross-actor client functions stay at module level and gain one
      `!emitrust.mut_ref` parameter per listed actor; their calls to arms
      become `emitrust.method_call` on the dereferenced parameter, and
      calls to other cross clients pass the parameters through (Rust's
      implicit reborrow);
    - the driver (`c_main`) constructs each actor as a defaulted named
      local, materializes non-default field initializers as
      post-construction member assigns, calls arms as method calls on the
      local, and takes a fresh `&mut` per cross-client call; globals of
      driver-only actors become named driver locals carrying the global's
      initializer;
    - whole-global snapshot round-trips (`variable` filled from a
      `global_load` and written back by a `global_store`) collapse to
      direct member access — the separately-tested elision cleanup;
    - the lifted `emitrust.global` ops are deleted.

    The pass is a NO-OP on a module carrying none of the attributes, which
    is every module produced without `--actor-lift`. An actor whose globals
    have any IR use the pass cannot rewrite is demoted on the spot with a
    warning and left completely untouched (the safety-net veto).
  }];
  let dependentDialects = ["::mlir::emitrust::EmitRustDialect"];
}

//===----------------------------------------------------------------------===//
// EmitRustActorThread
//===----------------------------------------------------------------------===//

def EmitRustActorThread : Pass<"emitrust-actor-thread", "ModuleOp"> {
  let summary = "Rewrite lifted actors to the threaded-runtime shape (FR-62)";
  let description = [{
    FR-62 slice 5b: rewrites each actor listed in the driver-computed
    `emitrust.actor_thread` module attribute (see
    EmitRust/Conversion/ActorThread.h for the contract) from the
    emitrust-actor-lift output shape into the threaded driver shape — runs
    strictly AFTER emitrust-actor-lift at the pipeline tail:

    - the driver's direct member/subscript accesses of the actor local
      become synthesized `get_<field>`/`set_<field>` accessor methods
      (ordinary impl funcs, scalar and scalar-array-element forms) called
      at the original access sites, order-preserving;
    - the actor local is retyped to the opaque `<Actor>Handle` and
      constructed by `<Actor>Handle::spawn(<Actor>::default())`; slice-4
      post-construction member-assign initializers become `set_` calls
      through the same accessor rewrite;
    - `.shutdown()` is inserted before every driver return (graceful join +
      panic-provenance propagation, the E3 clean form);
    - one `emitrust.actor_runtime` anchor per actor, placed after the impl,
      carries the mode for the emitter's runtime synthesis.

    A listed actor whose struct_def, impl, or constructing driver local is
    absent is skipped silently (it was lift-demoted — never lifted, so it
    keeps today's thread_local form). A lifted-but-thread-ineligible actor
    (cross-client `!emitrust.mut_ref`, unsendable method signature, driver
    access with no accessor form, accessor-name collision) stays lifted
    same-thread with a located
    `warning: actor plan: <actor> stays same-thread: <reason>`;
    demote-from-threading is not demote-from-lifting. The pass is a NO-OP
    on a module without the attribute.
  }];
  let dependentDialects = ["::mlir::emitrust::EmitRustDialect"];
}

#endif // EMITRUST_CONVERSION_PASSES
