# The EmitRust dialect library: dialect registration, ops (verifiers and
# custom parsers/printers), integer range inference, types, and attributes.
add_mlir_dialect_library(MLIREmitRustDialect
  EmitRustAttributes.cpp
  EmitRustDialect.cpp
  EmitRustOps.cpp
  EmitRustRangeInference.cpp
  EmitRustTypes.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/EmitRust

  DEPENDS
  MLIREmitRustOpsIncGen
  MLIREmitRustAttributesIncGen
  MLIREmitRustEnumsIncGen

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRSupport
  MLIRCallInterfaces
  MLIRControlFlowInterfaces
  MLIRFunctionInterfaces
  MLIRInferIntRangeCommon
  MLIRInferIntRangeInterface
  MLIRInferTypeOpInterface
  MLIRSideEffectInterfaces
  )
