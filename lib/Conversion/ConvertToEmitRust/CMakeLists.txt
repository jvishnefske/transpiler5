# The composite conversion library: convert-to-emitrust runs every
# X-to-EmitRust pattern set under one partial conversion,
# emitrust-range-refinement-check differentially re-derives integer ranges
# across that pipeline, and emitrust-value-identity-check differentially
# interprets both stages concretely (the lost-copy class).
# FR-130: LoweringPipeline.cpp is the ONE definition of the pinned
# import-to-emitrust pipeline both drivers run, so this library also links the
# two stages that live outside it (MLIRLowerContainers and the FR-52
# MLIREmitRustLowerExternalRequirements) plus the upstream mem2reg/canonicalize
# (MLIRTransforms) and lift-cf-to-scf (MLIRControlFlowToSCF) passes it names.
add_mlir_conversion_library(MLIRConvertToEmitRust
  ConvertToEmitRust.cpp
  LoweringPipeline.cpp
  RangeRefinementCheck.cpp
  ValueIdentityCheck.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/EmitRust/Conversion

  DEPENDS
  MLIREmitRustConversionPassIncGen

  LINK_LIBS PUBLIC
  MLIRAnalysis
  MLIRUBToEmitRust
  MLIRArithToEmitRust
  MLIRFuncToEmitRust
  MLIRSCFToEmitRust
  MLIRLowerContainers
  MLIREmitRustLowerExternalRequirements
  MLIRControlFlowToSCF
  MLIRTransforms
  MLIREmitRustDialect
  MLIRArithDialect
  MLIRControlFlowDialect
  MLIRFuncDialect
  MLIRMemRefDialect
  MLIRSCFDialect
  MLIRUBDialect
  MLIRPass
  MLIRTransformUtils
  )
