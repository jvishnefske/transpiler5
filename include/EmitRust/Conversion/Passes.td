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

#endif // EMITRUST_CONVERSION_PASSES
