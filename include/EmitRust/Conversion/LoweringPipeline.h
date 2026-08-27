//===- LoweringPipeline.h - The pinned lowering pipeline --------*- C++ -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// FR-130: declares `buildLoweringPipeline`, the ONE definition of the pinned
/// import-to-emitrust lowering pipeline every driver runs.
///
/// It used to be hand-copied into emitrust-cc and emitrust-clang, and the two
/// copies had diverged with no test able to see the difference. The stage list
/// now lives in one place and the only permitted divergence is the explicit
/// `LoweringPipelineOptions` fields below.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_CONVERSION_LOWERINGPIPELINE_H
#define EMITRUST_CONVERSION_LOWERINGPIPELINE_H

#include "mlir/Pass/PassManager.h"

namespace mlir {
namespace emitrust {

/// FR-130: the two OPTIONAL stages of the pinned lowering pipeline. Every
/// other stage is unconditional, so this struct is the complete statement of
/// how one driver's pipeline may differ from another's -- which is the point:
/// a difference that used to be an accident of copy-paste is now a named,
/// defaulted, reviewable decision.
struct LoweringPipelineOptions {
  /// Run emitrust-range-refinement-check after the SCF lift (emitrust-cc's
  /// `--check-range-refinement`): differentially re-derives integer ranges
  /// across the conversion. Off by default -- it is a verification stage, not
  /// a lowering stage.
  bool checkRangeRefinement = false;

  /// FR-134: run emitrust-canonical-roundtrip FIRST, before any lowering
  /// stage (emitrust-cc's `--check-canonical-roundtrip`): certifies that the
  /// FRONT END's module survives a round-trip through MLIR's canonical
  /// generic form unchanged. Off by default -- it is a verification stage,
  /// not a lowering stage, and it mutates nothing, so turning it on can only
  /// pass silently or fail the compilation loudly.
  bool canonicalRoundTrip = false;

  /// Run emitrust-lower-external-requirements (FR-52) after the conversion.
  /// A no-op unless the importer marked an unresolved external, which is why
  /// enabling it leaves every existing crate byte-identical.
  ///
  /// Off by default because the default consumer is an FR-58 SHARD, not a
  /// final crate: emitrust-clang imports with `deferExternals` (see design.md
  /// FR-57a, "defer takes precedence over the FR-52 trait policy"), so its
  /// unresolved externals are deliberately left as `emitrust.extern_decl`
  /// link obligations for the link step to resolve. Lowering them to the
  /// trait per-shard would resolve them too early. emitrust-cc, which emits a
  /// FINAL crate, turns this on.
  bool lowerExternalRequirements = false;
};

/// FR-130: appends the pinned import-to-emitrust lowering pipeline to `pm`.
///
/// The stage order is load-bearing and is stated once, here, rather than in
/// each driver:
///  0. [optional] emitrust-canonical-roundtrip BEFORE everything, so what it
///     certifies is the FRONT END's output -- the point at which a failure is
///     still attributable to the importer rather than to a lowering stage. It
///     mutates nothing.
///  1. emitrust-lower-containers FIRST, before mem2reg promotes the free
///     cursor, so all downstream lowering is identical to inlining the FR-39
///     node pool directly.
///  2. mem2reg + canonicalize, then lift-cf-to-scf + canonicalize: the
///     structured-control-flow shape the emitrust conversion expects.
///  3. [optional] emitrust-range-refinement-check -- differential, and it
///     clones internally, so the pipeline continues unchanged after it.
///  4. convert-to-emitrust.
///  5. [optional] emitrust-lower-external-requirements STRICTLY AFTER the
///     conversion, because the FR-52 trait is expressed in the EMITTED names
///     and in the opaque string callees the conversion produces.
///
/// Note which STANDARD simplification passes are deliberately absent: `cse`,
/// `sccp`, `loop-invariant-code-motion` and `remove-dead-values` were each
/// measured here and are net-negative, zero, or crashing -- see the recorded
/// NO-GO in design.md FR-130 before adding one.
void buildLoweringPipeline(OpPassManager &pm,
                           const LoweringPipelineOptions &options = {});

/// FR-130: registers `buildLoweringPipeline` as the `emitrust-lowering`
/// textual pass pipeline, so emitrust-opt can run the drivers' exact pipeline
/// and lit can pin its behaviour directly instead of only through a driver.
void registerEmitRustLoweringPipeline();

} // namespace emitrust
} // namespace mlir

#endif // EMITRUST_CONVERSION_LOWERINGPIPELINE_H
