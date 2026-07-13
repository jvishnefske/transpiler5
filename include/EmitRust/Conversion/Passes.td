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
      and a nested `emitrust.if`/else chain comparing the discriminator
      against each case value, with the default region as the final else;
      the regions assign the bindings at their `scf.yield`.
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

#endif // EMITRUST_CONVERSION_PASSES
