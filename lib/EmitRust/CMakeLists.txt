# The EmitRust dialect library: dialect registration, ops (verifiers and
# custom parsers/printers), types, and attributes.
add_mlir_dialect_library(MLIREmitRustDialect
  EmitRustAttributes.cpp
  EmitRustDialect.cpp
  EmitRustOps.cpp
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
  MLIRInferTypeOpInterface
  MLIRSideEffectInterfaces
  )
