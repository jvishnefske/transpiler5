//===- DifferentialStages.h - Shared differential-checker scaffold -*-C++-*-=//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Shared input-stage scaffold of the differential verification passes
/// (emitrust-range-refinement-check and emitrust-value-identity-check).
/// Both passes accept the same two input modes:
///
/// - Primary: a plain module. The pass analyzes it as the "before" stage,
///   clones it, runs convert-to-emitrust on the clone in a nested pass
///   manager, and treats the converted clone as the "after" stage.
/// - Secondary: a module holding exactly two nested `builtin.module` ops
///   tagged `emitrust.stage = "before"` and `emitrust.stage = "after"`,
///   compared directly with no internal conversion.
///
/// This header factors the mode detection and the clone-and-convert step
/// so the two checkers cannot drift apart.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_LIB_CONVERSION_CONVERTTOEMITRUST_DIFFERENTIALSTAGES_H
#define EMITRUST_LIB_CONVERSION_CONVERTTOEMITRUST_DIFFERENTIALSTAGES_H

#include "EmitRust/Conversion/ConvertToEmitRust.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Pass/PassManager.h"

#include <optional>

namespace mlir {
namespace emitrust {

/// The pre-/post-conversion module pair of the secondary input mode.
struct StagedModulePair {
  ModuleOp before;
  ModuleOp after;
};

/// Detects the secondary input mode: returns the nested "before"/"after"
/// pair when `module` holds exactly two nested `builtin.module` ops tagged
/// with an `emitrust.stage` string attribute covering both stages, and
/// `std::nullopt` otherwise (the caller falls back to the primary mode).
inline std::optional<StagedModulePair> detectStagedModulePair(ModuleOp module) {
  ModuleOp before;
  ModuleOp after;
  unsigned stagedCount = 0;
  for (Operation &op : module.getBody()->getOperations()) {
    auto nested = dyn_cast<ModuleOp>(op);
    if (!nested)
      continue;
    auto stage = nested->getAttrOfType<StringAttr>("emitrust.stage");
    if (!stage)
      continue;
    ++stagedCount;
    if (stage.getValue() == "before")
      before = nested;
    else if (stage.getValue() == "after")
      after = nested;
  }
  if (stagedCount == 2 && before && after)
    return StagedModulePair{before, after};
  return std::nullopt;
}

/// Primary-mode "after" stage derivation: clones `module` and runs the
/// convert-to-emitrust pipeline on the clone in a nested pass manager.
/// Returns the converted clone, or failure when the internal pipeline
/// fails (the caller emits its own located diagnostic).
inline FailureOr<OwningOpRef<ModuleOp>> cloneAndConvert(ModuleOp module) {
  OwningOpRef<ModuleOp> converted(module.clone());
  PassManager pipeline(module.getContext(), ModuleOp::getOperationName());
  pipeline.addPass(createConvertToEmitRust());
  if (failed(pipeline.run(*converted)))
    return failure();
  return converted;
}

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_LIB_CONVERSION_CONVERTTOEMITRUST_DIFFERENTIALSTAGES_H
