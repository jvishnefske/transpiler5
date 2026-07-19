//===- RangeRefinementCheck.cpp - Differential range checker --------------===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements the emitrust-range-refinement-check pass: a pipeline-level
/// differential abstract interpretation. The pass derives integer value
/// ranges (`IntegerRangeAnalysis` over `InferIntRangeInterface`) for the
/// module before and after the convert-to-emitrust pipeline, matches
/// observation points — `emitrust.call_opaque "print!"` operands and
/// function return operands, keyed by (function symbol, per-function
/// occurrence index, operand index) — and emits a located hard error when
/// a pre/post range pair is disjoint under BOTH the signed and the
/// unsigned interpretation. Containment failures (post not within pre,
/// e.g. a memory load that is TOP after conversion) are counted in a pass
/// statistic but stay silent; uninitialized or absent lattice values are
/// treated as TOP. The pass never modifies the IR.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Conversion/RangeRefinementCheck.h"

#include "DifferentialStages.h"
#include "EmitRust/Conversion/ConvertToEmitRust.h"
#include "EmitRust/EmitRustOps.h"
#include "mlir/Analysis/DataFlow/IntegerRangeAnalysis.h"
#include "mlir/Analysis/DataFlow/Utils.h"
#include "mlir/Analysis/DataFlowFramework.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <optional>
#include <string>
#include <tuple>

namespace mlir {
namespace emitrust {
#define GEN_PASS_DEF_EMITRUSTRANGEREFINEMENTCHECK
#include "EmitRust/Conversion/Passes.h.inc"
} // namespace emitrust
} // namespace mlir

using namespace mlir;
using namespace mlir::emitrust;

namespace {

/// Identifies one observable operand: the enclosing function symbol,
/// whether the operand belongs to a return terminator (false: a
/// `call_opaque "print!"`), the per-function occurrence index of that
/// observation op, and the operand index within it. The tuple ordering
/// makes diagnostic emission deterministic.
using ObservationKey = std::tuple<std::string, bool, unsigned, unsigned>;

/// One observed operand: the observation op (used as the diagnostic
/// location) and the derived range, where `std::nullopt` is TOP (the
/// lattice value was absent, uninitialized, or of unusable width).
struct Observation {
  Operation *op;
  std::optional<ConstantIntRanges> range;
};

using ObservationMap = std::map<ObservationKey, Observation>;

} // namespace

/// Returns the range the solver derived for `value`, or std::nullopt (TOP)
/// when the lattice value is absent, uninitialized, or does not match the
/// value type's storage width.
static std::optional<ConstantIntRanges> lookupRange(DataFlowSolver &solver,
                                                    Value value) {
  auto *lattice =
      solver.lookupState<dataflow::IntegerValueRangeLattice>(value);
  if (!lattice || lattice->getValue().isUninitialized())
    return std::nullopt;
  unsigned width = ConstantIntRanges::getStorageBitwidth(value.getType());
  const ConstantIntRanges &range = lattice->getValue().getValue();
  if (width == 0 || range.umin().getBitWidth() != width)
    return std::nullopt;
  return range;
}

/// Collects the observation points of every function nested under
/// `moduleLike` into `observations`, reading ranges from `solver`.
/// Occurrence indices restart at zero in each function and count the
/// `call_opaque "print!"` ops and the return terminators separately, in
/// walk (program) order.
static void collectObservations(Operation *moduleLike, DataFlowSolver &solver,
                                ObservationMap &observations) {
  moduleLike->walk([&](FunctionOpInterface fn) {
    StringRef symbol = SymbolTable::getSymbolName(fn).getValue();
    unsigned printIndex = 0;
    unsigned returnIndex = 0;
    fn->walk([&](Operation *op) {
      bool isReturn = isa<func::ReturnOp, emitrust::ReturnOp>(op);
      if (auto call = dyn_cast<emitrust::CallOpaqueOp>(op);
          !isReturn && (!call || call.getCallee() != "print!"))
        return;
      unsigned occurrence = isReturn ? returnIndex++ : printIndex++;
      for (auto [index, operand] : llvm::enumerate(op->getOperands()))
        observations.insert(
            {ObservationKey(symbol.str(), isReturn, occurrence,
                            static_cast<unsigned>(index)),
             Observation{op, lookupRange(solver, operand)}});
    });
  });
}

/// Runs the integer range analysis over `moduleLike` and fills
/// `observations` with the ranges at its observation points.
static LogicalResult analyzeStage(Operation *moduleLike,
                                  ObservationMap &observations) {
  DataFlowSolver solver;
  dataflow::loadBaselineAnalyses(solver);
  solver.load<dataflow::IntegerRangeAnalysis>();
  if (failed(solver.initializeAndRun(moduleLike)))
    return failure();
  collectObservations(moduleLike, solver, observations);
  return success();
}

/// Renders the signed interpretation of `range` as `[smin, smax]`.
static std::string formatRange(const ConstantIntRanges &range) {
  std::string text;
  llvm::raw_string_ostream os(text);
  os << "[";
  range.smin().print(os, /*isSigned=*/true);
  os << ", ";
  range.smax().print(os, /*isSigned=*/true);
  os << "]";
  return text;
}

namespace {

/// The emitrust-range-refinement-check pass. See the TableGen description
/// in Passes.td for the input modes, verdict semantics, and diagnostic
/// contract.
struct EmitRustRangeRefinementCheck
    : public emitrust::impl::EmitRustRangeRefinementCheckBase<
          EmitRustRangeRefinementCheck> {
  /// Detects the input mode, derives the pre- and post-conversion
  /// observation maps, and compares them.
  void runOnOperation() override {
    ModuleOp module = getOperation();

    // Secondary mode: exactly two nested modules tagged with
    // emitrust.stage = "before" / "after".
    std::optional<StagedModulePair> staged = detectStagedModulePair(module);

    ObservationMap preObservations;
    ObservationMap postObservations;
    if (staged) {
      if (failed(analyzeStage(staged->before, preObservations)) ||
          failed(analyzeStage(staged->after, postObservations)))
        return signalPassFailure();
    } else {
      // Primary mode: analyze the input, then convert a clone and analyze
      // the result.
      if (failed(analyzeStage(module, preObservations)))
        return signalPassFailure();
      FailureOr<OwningOpRef<ModuleOp>> converted = cloneAndConvert(module);
      if (failed(converted)) {
        module.emitError("range refinement check: internal "
                         "convert-to-emitrust pipeline failed");
        return signalPassFailure();
      }
      if (failed(analyzeStage(**converted, postObservations)))
        return signalPassFailure();
    }

    compare(preObservations, postObservations);
    markAllAnalysesPreserved();
  }

  /// Compares matched observation points: emits the pinned located error
  /// (and fails the pass) for every pair whose ranges are disjoint under
  /// both the signed and the unsigned interpretation, and counts silent
  /// containment failures in the pass statistic.
  void compare(const ObservationMap &preObservations,
               const ObservationMap &postObservations) {
    for (const auto &[key, pre] : preObservations) {
      auto match = postObservations.find(key);
      if (match == postObservations.end())
        continue;
      const Observation &post = match->second;
      // TOP on either side intersects everything.
      if (!pre.range || !post.range)
        continue;
      if (pre.range->umin().getBitWidth() != post.range->umin().getBitWidth())
        continue;
      ConstantIntRanges common = pre.range->intersection(*post.range);
      bool unsignedEmpty = common.umin().ugt(common.umax());
      bool signedEmpty = common.smin().sgt(common.smax());
      if (unsignedEmpty && signedEmpty) {
        const auto &[symbol, isReturn, occurrence, operandIndex] = key;
        (void)occurrence;
        pre.op->emitError()
            << "range refinement violation: '" << symbol << "' "
            << (isReturn ? "return value " : "print operand ") << operandIndex
            << " has pre-conversion range " << formatRange(*pre.range)
            << " disjoint from post-conversion range "
            << formatRange(*post.range);
        signalPassFailure();
        continue;
      }
      // Silent containment check: is post within pre under both
      // interpretations?
      bool contained = post.range->umin().uge(pre.range->umin()) &&
                       post.range->umax().ule(pre.range->umax()) &&
                       post.range->smin().sge(pre.range->smin()) &&
                       post.range->smax().sle(pre.range->smax());
      if (!contained)
        ++numContainmentFailures;
    }
  }
};

} // namespace
