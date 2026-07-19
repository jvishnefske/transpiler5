# The composite conversion library: convert-to-emitrust runs every
# X-to-EmitRust pattern set under one partial conversion, and
# emitrust-range-refinement-check differentially re-derives integer ranges
# across that pipeline.
add_mlir_conversion_library(MLIRConvertToEmitRust
  ConvertToEmitRust.cpp
  RangeRefinementCheck.cpp

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
