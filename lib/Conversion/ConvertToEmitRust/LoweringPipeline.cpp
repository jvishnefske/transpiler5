//===- LoweringPipeline.cpp - The pinned lowering pipeline ------*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// FR-130: the ONE definition of the import-to-emitrust lowering pipeline.
///
/// It used to be hand-copied into emitrust-cc and emitrust-clang, and the two
/// copies had drifted: emitrust-clang was missing both the optional
/// range-refinement check and the FR-52 external-requirement lowering, with no
/// test that could see the difference. The pipeline is now stated once and
/// both drivers call it, so the only way they can differ is through the
/// explicit `LoweringPipelineOptions` fields.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Conversion/LoweringPipeline.h"

#include "EmitRust/Conversion/ConvertToEmitRust.h"
#include "EmitRust/Conversion/LowerContainers.h"
#include "EmitRust/Conversion/LowerExternalRequirements.h"
#include "EmitRust/Conversion/CanonicalRoundTrip.h"
#include "EmitRust/Conversion/RangeRefinementCheck.h"

#include "mlir/Conversion/ControlFlowToSCF/ControlFlowToSCF.h"
#include "mlir/Transforms/Passes.h"

namespace mlir {
namespace emitrust {

void buildLoweringPipeline(OpPassManager &pm,
                           const LoweringPipelineOptions &options) {
  // FR-134: BEFORE every lowering stage, so what it certifies is the FRONT
  // END's module -- at this point a round-trip failure is still attributable
  // to the importer. Mutates nothing: it either passes silently or fails the
  // compile with its located diagnostic.
  if (options.canonicalRoundTrip)
    pm.addPass(createEmitRustCanonicalRoundTrip());
  // Lower the high-level container ops (the FR-39 node pool) to the concrete
  // `[T;CAP]` array + cursor shape FIRST, before mem2reg promotes the cursor,
  // so all downstream lowering is identical to inlining the pool directly.
  pm.addPass(createEmitRustLowerContainers());
  pm.addPass(mlir::createMem2Reg());
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(mlir::createLiftControlFlowToSCFPass());
  pm.addPass(mlir::createCanonicalizerPass());
  // Differential, and it clones internally -- so the main pipeline continues
  // unchanged after it; a violation fails the compile with the pass's located
  // diagnostic.
  if (options.checkRangeRefinement)
    pm.addPass(createEmitRustRangeRefinementCheck());
  pm.addPass(createConvertToEmitRust());
  // FR-52: strictly after the conversion, because the trait is expressed in
  // the EMITTED names and in the opaque string callees the conversion
  // produces. A no-op unless the importer marked an unresolved external,
  // which is why every existing crate stays byte-identical.
  if (options.lowerExternalRequirements)
    pm.addPass(createEmitRustLowerExternalRequirements());
}

void registerEmitRustLoweringPipeline() {
  PassPipelineRegistration<>(
      "emitrust-lowering",
      "The pinned import-to-emitrust lowering pipeline the drivers run",
      [](OpPassManager &pm) { buildLoweringPipeline(pm); });
}

} // namespace emitrust
} // namespace mlir
