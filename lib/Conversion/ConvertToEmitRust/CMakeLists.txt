# The composite conversion library: convert-to-emitrust runs every
# X-to-EmitRust pattern set under one partial conversion,
# emitrust-range-refinement-check differentially re-derives integer ranges
# across that pipeline, and emitrust-value-identity-check differentially
# interprets both stages concretely (the lost-copy class).
add_mlir_conversion_library(MLIRConvertToEmitRust
  ConvertToEmitRust.cpp
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
